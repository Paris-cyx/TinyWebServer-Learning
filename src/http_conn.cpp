#include "http_conn.h"
#include <sys/epoll.h>

// 定义静态成员
int HttpConn::m_epollfd = -1;
int HttpConn::m_user_count = 0;

// 设置 fd 为非阻塞 (ET模式必备)
void setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
}

// 向 Epoll 添加需要监听的 fd
void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    // EPOLLIN: 读事件
    // EPOLLET: 边缘触发 (Edge Trigger)，这是高性能的关键！
    // EPOLLRDHUP: 对方断开连接
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if(one_shot) {
        // EPOLLONESHOT: 防止多线程同时操作同一个 socket (非常重要！)
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从 Epoll 移除 fd
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// =================================================================
// 核心逻辑实现
// =================================================================

void HttpConn::init(int sockfd, const sockaddr_in& addr) {
    m_sockfd = sockfd;
    m_address = addr;
    
    // 端口复用 (在 server.cpp 里设置也行，这里保险起见)
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 添加到 epoll 监控
    addfd(m_epollfd, sockfd, true); 
    m_user_count++;

    init_parse_state();
}

void HttpConn::init_parse_state() {
    m_check_state = CHECK_STATE_REQUESTLINE; // 初始状态：查请求行
    m_method = GET;
    m_read_idx = 0;
    m_checked_idx = 0;
    m_start_line = 0;
    memset(m_read_buf, 0, sizeof(m_read_buf));
    memset(m_real_file, 0, sizeof(m_real_file));
}

void HttpConn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 循环读取数据，直到无数据可读 (ET模式必须一次性读完)
bool HttpConn::read_once() {
    if(m_read_idx >= sizeof(m_read_buf)) return false;
    
    int bytes_read = 0;
    while(true) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, 
                          sizeof(m_read_buf) - m_read_idx, 0);
        if(bytes_read == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // 读完了
            }
            return false; // 出错
        }
        else if(bytes_read == 0) {
            return false; // 对方关闭连接
        }
        m_read_idx += bytes_read;
    }
    return true;
}

// ============================================
// 状态机核心：解析每一行
// ============================================
HttpConn::LINE_STATUS HttpConn::parse_line() {
    char temp;
    // m_checked_idx 指向当前正在分析的字符
    // m_read_idx 指向缓冲区数据的末尾
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        
        // 如果读到回车符 \r
        if (temp == '\r') {
            // 如果 \r 是最后一个字符，说明还没收完，返回 LINE_OPEN (行不完整)
            if ((m_checked_idx + 1) == m_read_idx) {
                return LINE_OPEN;
            }
            // 如果下一个是 \n，说明读到了一行完整的 \r\n
            else if (m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx++] = '\0'; // 把 \r 变成字符串结束符
                m_read_buf[m_checked_idx++] = '\0'; // 把 \n 变成字符串结束符
                return LINE_OK;
            }
            return LINE_BAD; // 语法错误
        }
        // 如果读到换行符 \n (一般是接在 \r 后面的)
        else if (temp == '\n') {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r')) {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN; // 没找到换行符，说明这行还没发完
}

// ============================================
// 状态机核心：解析请求行 (GET /index.html HTTP/1.1)
// ============================================
HttpConn::HTTP_CODE HttpConn::parse_request_line(char* text) {
    // text 目前是 "GET /index.html HTTP/1.1"
    
    // 1. 查找第一个空格 (GET 后面那个)
    char* url = strpbrk(text, " \t");
    if (!url) return BAD_REQUEST;
    
    *url++ = '\0'; // 把空格变成 \0，分割字符串

    // 2. 判断方法 (text 现在指向 "GET")
    char* method = text;
    if (strcasecmp(method, "GET") == 0) m_method = GET;
    else return BAD_REQUEST;

    // 3. 查找 HTTP 版本 (跳过中间的空格)
    url += strspn(url, " \t");
    
    char* version = strpbrk(url, " \t");
    if (!version) return BAD_REQUEST;
    *version++ = '\0';
    
    // 检查版本号
    if (strcasecmp(version, "HTTP/1.1") != 0) return BAD_REQUEST;

    // 4. 处理 URL
    // 如果 URL 是 http://localhost:8080/index.html，要去掉前面的 http://...
    if (strncasecmp(url, "http://", 7) == 0) {
        url += 7;
        url = strchr(url, '/');
    }
    if (!url || url[0] != '/') return BAD_REQUEST;
    
    // 默认请求 / 时，映射到 /index.html
    if (strlen(url) == 1) strcat(url, "index.html");

    cout << ">>> 解析成功: Method=GET, URL=" << url << endl;
    
    // 状态转移！解析完请求行，下一步状态是 CHECK_STATE_HEADER
    m_check_state = CHECK_STATE_HEADER; 
    return NO_REQUEST; // 还没结束，继续解析
}

HttpConn::HTTP_CODE HttpConn::parse_headers(char* text) {
    // 遇到空行，说明头部解析完毕
    if (text[0] == '\0') {
        // 如果有 Content-Length，应该转移到 CHECK_STATE_CONTENT
        // 这里简化：只有 GET，遇到空行就认为请求解析完成了
        return GET_REQUEST; 
    }
    // 这里可以解析 Host, Connection 等字段，暂时略过，直接打印
    // cout << "Header: " << text << endl;
    return NO_REQUEST;
}

// ============================================
// 主驱动函数
// ============================================
HttpConn::HTTP_CODE HttpConn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    // 循环条件：
    // 1. 正在解析内容 (CONTENT 状态不按行读)
    // 2. 或者 解析出一行完整的数据 (LINE_OK)
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
           || ((line_status = parse_line()) == LINE_OK)) 
    {
        // text 指向这一行的开头
        text = m_read_buf + m_start_line;
        m_start_line = m_checked_idx; // 更新下一行的起始位置

        // 状态机 switch
        switch (m_check_state) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) return BAD_REQUEST;
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers(text);
                if (ret == GET_REQUEST) return GET_REQUEST; // 成功！
                break;
            }
            case CHECK_STATE_CONTENT: {
                // 暂时不处理 POST
                return GET_REQUEST;
            }
            default: return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST; // 数据不完整，继续等数据
}

// 处理请求的入口 (由线程池调用)
void HttpConn::process() {
    HTTP_CODE read_ret = process_read();
    
    if (read_ret == NO_REQUEST) {
        // 请求不完整，需要继续接收 (注册 EPOLLIN)
        epoll_event event;
        event.data.fd = m_sockfd;
        event.events = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
        epoll_ctl(m_epollfd, EPOLL_CTL_MOD, m_sockfd, &event);
        return;
    }
    
    if (read_ret == GET_REQUEST) {
        // 解析成功！这里应该去准备文件
        // 为了演示，我们先简单发个固定的成功响应
        cout << ">>> HTTP 请求完整，准备发送响应..." << endl;
        
        const char* resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<h1>Parsed by State Machine!</h1>";
        send(m_sockfd, resp, strlen(resp), 0);
        close_conn(); // 短连接，发完就关
    }
    else {
        // 出错
        close_conn();
    }
}