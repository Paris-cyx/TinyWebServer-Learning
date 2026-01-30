#include "http_conn.h"
#include "sql_conn_pool.h"
#include <sys/epoll.h>
#include <stdarg.h>
#include <map>
#include <iostream>
#include "log.h" 

using namespace std;

const char* doc_root = "resources";

int HttpConn::m_epollfd = -1;
int HttpConn::m_user_count = 0;

map<string, string> users;
mutex m_lock;

const char* get_mime_type(const char* name) {
    if (strstr(name, ".html")) return "text/html";
    else if (strstr(name, ".css")) return "text/css";
    else if (strstr(name, ".js")) return "text/javascript";
    else if (strstr(name, ".png")) return "image/png";
    else if (strstr(name, ".jpg") || strstr(name, ".jpeg")) return "image/jpeg";
    else if (strstr(name, ".gif")) return "image/gif";
    else if (strstr(name, ".mp4")) return "video/mp4";  
    else if (strstr(name, ".avi")) return "video/x-msvideo";
    else return "text/plain";
}

void setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
}

void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if(one_shot) event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void HttpConn::initmysql_result(SqlConnPool* connPool) {
    MYSQL* mysql = NULL;
    SqlConnRAII mysqlcon(&mysql, connPool);

    if (mysql_query(mysql, "SELECT username, passwd FROM user")) {
        // 如果表还没建，这里会报错，但不影响编译
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    MYSQL_RES* result = mysql_store_result(mysql);
    // 如果查询失败（比如没表），result 为空，跳过循环
    if (!result) return; 

    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

void HttpConn::init(int sockfd, const sockaddr_in& addr) {
    m_sockfd = sockfd;
    m_address = addr;
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd(m_epollfd, sockfd, true); 
    m_user_count++;
    init_parse_state();
}

void HttpConn::init_parse_state() {
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_method = GET;
    m_read_idx = 0;
    m_checked_idx = 0;
    m_start_line = 0;
    m_write_idx = 0;
    m_content_length = 0;
    
    m_url = 0;
    m_version = 0;
    m_host = 0;
    m_string = nullptr;
    m_file_address = 0;
    m_linger = false;
    
    memset(m_read_buf, 0, sizeof(m_read_buf));
    memset(m_write_buf, 0, sizeof(m_write_buf));

    // 【新增】在这里初始化 Cookie 状态 (每次请求开始前重置)
    // =======================================================
    m_set_cookie = 0;         // 默认不发 Set-Cookie
    m_cookie_is_login = false;// 默认没有登录

    // 【新增】文件上传变量初始化
    memset(m_boundary, 0, sizeof(m_boundary));
    m_is_multipart = false;
    memset(m_upload_filename, 0, sizeof(m_upload_filename));
    m_file_content = nullptr;

    // 【新增】在这里初始化 JSON 状态
    m_is_json = false;
    m_json_string = nullptr;
}

void HttpConn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

bool HttpConn::read_once() {
    if(m_read_idx >= sizeof(m_read_buf)) return false;
    int bytes_read = 0;
    while(true) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, sizeof(m_read_buf) - m_read_idx, 0);
        if(bytes_read == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) break;
            return false;
        } else if(bytes_read == 0) return false;
        m_read_idx += bytes_read;
    }
    return true;
}

HttpConn::LINE_STATUS HttpConn::parse_line() {
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') {
            if ((m_checked_idx + 1) == m_read_idx) return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if (temp == '\n') {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r')) {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

HttpConn::HTTP_CODE HttpConn::parse_request_line(char* text) {
    m_url = strpbrk(text, " \t");
    if (!m_url) return BAD_REQUEST;
    *m_url++ = '\0';
    
    char* method = text;
    if (strcasecmp(method, "GET") == 0) m_method = GET;
    else if (strcasecmp(method, "POST") == 0) m_method = POST;
    else return BAD_REQUEST;
    
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version) return BAD_REQUEST;
    *m_version++ = '\0';
    if (strcasecmp(m_version, "HTTP/1.1") != 0) return BAD_REQUEST;
    
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/') return BAD_REQUEST;
    
    if (strlen(m_url) == 1) strcat(m_url, "index.html");
    
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

HttpConn::HTTP_CODE HttpConn::parse_headers(char* text) {
    if (text[0] == '\0') {
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST; 
    }
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else if (strncasecmp(text, "Cookie:", 7) == 0) {
        text += 7;
        text += strspn(text, " \t");
        // LOG_INFO("Cookie: %s", text); // 调试时可以打印看看
        
        // 简单粗暴的判断：只要 Cookie 里包含 "is_login=true" 字符串，就算登录了
        if (strstr(text, "is_login=true")) {
            m_cookie_is_login = true;
        }
    }
    else if (strncasecmp(text, "Content-Type:", 13) == 0) {
    text += 13;
    text += strspn(text, " \t");
    
    // 检查是不是 multipart/form-data
    if (strncasecmp(text, "multipart/form-data", 19) == 0) {
        m_is_multipart = true; // 标记为文件上传
        
        // 寻找 boundary 的位置
        // 格式通常是: multipart/form-data; boundary=----WebKitFormBoundary...
        char* boundary_pos = strstr(text, "boundary=");
        if (boundary_pos) {
            boundary_pos += 9; // 跳过 "boundary=" 这9个字符
            strcpy(m_boundary, boundary_pos); // 存下分界线
            }
        }
    }   
    return NO_REQUEST;
}

HttpConn::HTTP_CODE HttpConn::parse_content(char* text) {
    // 判断是否读取了完整的 Body
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        
        // 【新增】如果是 multipart 格式，调用专门的解析函数
        if (m_is_multipart) {
            return parse_multipart_content(text);
        }

        // 原有的普通 POST 处理逻辑 (比如登录注册)
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 【新增】解析文件上传的核心逻辑
HttpConn::HTTP_CODE HttpConn::parse_multipart_content(char* text) {
    // 1. 构造完整的 boundary 字符串 (格式通常是 --boundary)
    char boundary_str[200];
    snprintf(boundary_str, 200, "--%s", m_boundary);
    int boundary_len = strlen(boundary_str);

    // 2. 解析头部，获取文件名
    // 格式示例: Content-Disposition: form-data; name="file"; filename="avatar.jpg"
    
    // 跳过第一行 boundary
    char* start_headers = strstr(text, "\r\n");
    if (!start_headers) return BAD_REQUEST;
    start_headers += 2; // 跳过 \r\n

    // 查找 filename 字段
    char* filename_ptr = strstr(start_headers, "filename=\"");
    if (!filename_ptr) return BAD_REQUEST;
    filename_ptr += 10; // 跳过 filename=" 这10个字符

    // 找到文件名结束的引号
    char* end_filename = strchr(filename_ptr, '"');
    if (!end_filename) return BAD_REQUEST;
    
    // 提取文件名
    int name_len = end_filename - filename_ptr;
    if (name_len >= 99) name_len = 99;
    strncpy(m_upload_filename, filename_ptr, name_len);
    m_upload_filename[name_len] = '\0';
    
    LOG_INFO("Detected Upload File: %s", m_upload_filename);

    // 3. 寻找二进制数据的起始位置
    // 数据开始于头部之后的第一个空行 (\r\n\r\n)
    char* data_start = strstr(end_filename, "\r\n\r\n");
    if (!data_start) return BAD_REQUEST;
    data_start += 4; // 跳过 \r\n\r\n，指针现在指向图片数据的第一个字节

    // 4. 计算文件大小 (这是最难的一步)
    // 因为图片数据里可能包含 \0，所以不能用 strlen。
    // 我们必须根据 m_content_length (总长度) 减去头部和尾部来计算。
    
    int total_len = m_content_length;
    int header_offset = data_start - text; // 头部占用的长度
    int data_len_with_footer = total_len - header_offset; // 剩余部分长度(含尾部 boundary)

    // 尾部通常是: \r\n--boundary--\r\n
    // 我们通过暴力搜索找到尾部 boundary 的位置
    char* data_end = nullptr;
    for (int i = 0; i < data_len_with_footer - boundary_len; ++i) {
        // 查找特征: \r\n + boundary
        if (memcmp(data_start + i, "\r\n", 2) == 0) {
             if (memcmp(data_start + i + 2, boundary_str, boundary_len) == 0) {
                 data_end = data_start + i;
                 break;
             }
        }
    }
    if (!data_end) return BAD_REQUEST;

    int file_size = data_end - data_start;

    // 5. 保存文件到本地 (简单的文件 IO)
    // 假设保存到当前运行目录，或者你可以指定 resources/ 目录
    char file_path[256];
    // 这里为了演示方便，直接存到 server_core 同级目录下，文件名前加 upload_ 前缀
    snprintf(file_path, 256, "resources/upload_%s", m_upload_filename);

    FILE* fp = fopen(file_path, "wb"); // wb: 二进制写入模式
    if (!fp) {
        LOG_ERROR("Open file failed: %s", file_path);
        return INTERNAL_ERROR;
    }
    fwrite(data_start, 1, file_size, fp);
    fclose(fp);

    LOG_INFO("File Saved Successfully: %s (Size: %d bytes)", file_path, file_size);

    // 6. 修改跳转逻辑
    // 上传成功后，我们不想让用户看到白屏，而是跳回欢迎页或者成功页
    m_url = "/welcome.html"; 

    return GET_REQUEST;
}

HttpConn::HTTP_CODE HttpConn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
            || ((line_status = parse_line()) == LINE_OK)) {
        text = m_read_buf + m_start_line;
        m_start_line = m_checked_idx;
        switch (m_check_state) {
            case CHECK_STATE_REQUESTLINE:
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) return BAD_REQUEST;
                break;
            case CHECK_STATE_HEADER:
                ret = parse_headers(text);
                if (ret == GET_REQUEST) return do_request();
                break;
            case CHECK_STATE_CONTENT:
                ret = parse_content(text); 
                if (ret == GET_REQUEST) return do_request();
                line_status = LINE_OPEN;
                break;
            default: return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

HttpConn::HTTP_CODE HttpConn::do_request() {
    char path[200];
    strcpy(path, doc_root);
    int len = strlen(doc_root);
    
    if (!m_url) {
        return BAD_REQUEST;
    }

    // 1. 拦截未登录访问 (保持原样)
    if ((strcasecmp(m_url, "/welcome.html") == 0 || strcasecmp(m_url, "/media.html") == 0) 
        && !m_cookie_is_login) {
        strcpy(m_url, "/logError.html"); 
    }

    const char *p = strrchr(m_url, '/');
    
    // ========================================================
    // 2. 登录/注册逻辑 (主要修改这里)
    // ========================================================
    if (m_method == POST && (strcasecmp(m_url, "/2") == 0 || strcasecmp(m_url, "/3") == 0)) {
        
        // 【新增】标记这是一个 API 请求
        m_is_json = true;

        char name[100] = {0};
        char password[100] = {0};
        int i;
        
        // --- 解析部分 (完全保留你的代码) ---
        if (!m_string) return BAD_REQUEST;

        for (i = 5; m_string[i] != '&' && i < m_content_length; ++i) {
            name[i - 5] = m_string[i];
        }
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0' && m_string[i] != '&' && i < m_content_length; ++i, ++j) {
            password[j] = m_string[i];
        }
        password[j] = '\0';
        
        // 数据库检查 (完全保留你的代码)
        m_lock.lock();
        bool user_exists = (users.find(name) != users.end());
        m_lock.unlock();
        
        // --- 注册逻辑 (/3) ---
        if (strcasecmp(m_url, "/3") == 0) {
            if (!user_exists) {
                // 插入数据库 (完全保留你的 SQL 逻辑)
                char* sql_insert = (char*)malloc(sizeof(char) * 200);
                sprintf(sql_insert, "INSERT INTO user(username, passwd) VALUES('%s', '%s')", name, password);
                
                MYSQL* mysql = NULL;
                SqlConnRAII mysqlcon(&mysql, SqlConnPool::Instance());
                
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                if(!res) {
                    users[name] = password;
                    // 【修改点 A】注册成功：不再跳转 /index.html，而是返回 JSON
                    m_json_string = (char*)"{\"code\": 200, \"msg\": \"Reg Success\", \"url\": \"/index.html\"}";
                } else {
                    // 【修改点 B】数据库错误
                    m_json_string = (char*)"{\"code\": 500, \"msg\": \"DB Error\"}";
                }
                m_lock.unlock();
                free(sql_insert);
            } else {
                // 【修改点 C】用户已存在
                m_json_string = (char*)"{\"code\": 400, \"msg\": \"User Exist\"}";
            }
        }
        // --- 登录逻辑 (/2) ---
        else if (strcasecmp(m_url, "/2") == 0) {
            if (user_exists && users[name] == password) {
                // 【修改点 D】登录成功
                m_json_string = (char*)"{\"code\": 200, \"msg\": \"Login Success\", \"url\": \"/welcome.html\"}";
                
                // 必须设置 Cookie 状态
                m_set_cookie = 1;
                m_cookie_is_login = true; 
                LOG_INFO("Login Success: %s", name);
            } else {
                // 【修改点 E】登录失败
                m_json_string = (char*)"{\"code\": 401, \"msg\": \"Login Failed\"}";
            }
        }
        
        // 【关键】直接返回，不走下面的文件读取流程
        return GET_REQUEST; 
    }

    // ========================================================
    // 3. 静态文件处理 (保持原样)
    // 只有非 JSON 请求才会走到这里
    // ========================================================

    if (strcasecmp(m_url, "/") == 0) strcpy(m_url, "/index.html");

    m_real_file = new char[200];
    strncpy(m_real_file, path, 200);
    int path_len = strlen(path);
    strncpy(m_real_file + path_len, m_url, 200 - path_len);

    if (stat(m_real_file, &m_file_stat) < 0) return NO_RESOURCE;
    if (!(m_file_stat.st_mode & S_IROTH)) return FORBIDDEN_REQUEST;
    if (S_ISDIR(m_file_stat.st_mode)) return BAD_REQUEST;
    
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    
    return FILE_REQUEST;
}

void HttpConn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool HttpConn::write() {
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx + m_file_stat.st_size; 

    if (bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init_parse_state();
        return true;
    }

    while (1) {
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1) {
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        
        if (bytes_to_send <= 0) {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN); 
            if (m_linger) { 
                init_parse_state();
                return true;
            } else {
                return false;
            }
        }
    }
}

bool HttpConn::add_response(const char* format, ...) {
    // 【核心修复】增加 (int) 强制转换，解决 comparison 警告
    if (m_write_idx >= (int)sizeof(m_write_buf)) return false; 
    
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, sizeof(m_write_buf) - 1 - m_write_idx, format, arg_list);
    
    // 【核心修复】增加 (int) 强制转换
    if (len >= (int)(sizeof(m_write_buf) - 1 - m_write_idx)) return false; 
    
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool HttpConn::add_content(const char* content) {
    // 调用你现有的 add_response，把内容格式化进去
    return add_response("%s", content);
}

bool HttpConn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool HttpConn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
    
    // 【关键修复】Cookie 必须在 add_blank_line 之前发送！
    // 只有这样，它才属于 Header，浏览器才会识别并存储。
    if (m_set_cookie == 1) {
        add_response("Set-Cookie: is_login=true; Max-Age=3600; Path=/\r\n");
        m_set_cookie = 0; 
    }

    // 这一行必须是最后一句！它代表 Header 结束，Body 开始。
    add_blank_line(); 
    return true;
}
bool HttpConn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}
bool HttpConn::add_content_type() {
    // 1. 如果是 API 请求，返回 JSON
    if (m_is_json) {
        return add_response("Content-Type:%s\r\n", "application/json;charset=utf-8");
    }

    // 2. 如果是文件请求，根据后缀名判断类型
    // 获取文件名后缀 (比如 .jpg)
    const char* suffix = strrchr(m_url, '.');
    
    if (suffix != nullptr) {
        if (strcasecmp(suffix, ".html") == 0) return add_response("Content-Type:%s\r\n", "text/html");
        if (strcasecmp(suffix, ".css")  == 0) return add_response("Content-Type:%s\r\n", "text/css");
        if (strcasecmp(suffix, ".js")   == 0) return add_response("Content-Type:%s\r\n", "text/javascript");
        if (strcasecmp(suffix, ".jpg")  == 0 || strcasecmp(suffix, ".jpeg") == 0) 
            return add_response("Content-Type:%s\r\n", "image/jpeg");
        if (strcasecmp(suffix, ".png")  == 0) return add_response("Content-Type:%s\r\n", "image/png");
        if (strcasecmp(suffix, ".gif")  == 0) return add_response("Content-Type:%s\r\n", "image/gif");
        if (strcasecmp(suffix, ".mp4")  == 0) return add_response("Content-Type:%s\r\n", "video/mp4");
        // 如果想支持更多格式，可以在这里继续加
    }

    // 3. 默认兜底：依然是 HTML (或者用 text/plain)
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool HttpConn::add_blank_line() {
    return add_response("\r\n");
}

bool HttpConn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR:
            add_status_line(500, "Internal Server Error");
            // 原版这里可能没加 content，直接返回空 body 也是可以的
            break;
            
        case NO_RESOURCE:
            add_status_line(404, "Not Found");
            break;
            
        case FORBIDDEN_REQUEST:
            add_status_line(403, "Forbidden");
            break;

        // ======================================================
        // 【新增】 处理 API 请求 (JSON) 或 普通文本请求
        // 对应 do_request 中返回的 GET_REQUEST
        // ======================================================
        case GET_REQUEST:
            add_status_line(200, "OK");
            
            // 1. 如果是 JSON 模式 (我们在 do_request 里标记的)
            if (m_is_json && m_json_string) {
                // 添加头部 (Content-Length 等)
                // 注意：这里会调用 add_content_type，记得去第一步把那里改好
                if (!add_headers(strlen(m_json_string))) {
                    return false;
                }
                // 把 JSON 字符串拷贝到写缓冲区 m_write_buf
                if (!add_content(m_json_string)) {
                    return false;
                }
            } 
            // 2. 默认保底逻辑 (如果不是 JSON，发个空页面)
            else {
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string)) return false;
            }
            break;

        // ======================================================
        // 处理静态文件 (保持原样)
        // ======================================================
        case FILE_REQUEST:
            add_status_line(200, "OK");
            if (m_file_stat.st_size != 0) {
                add_headers(m_file_stat.st_size);
                
                // 核心差异：文件传输需要两块内存 (头 + 文件)
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address; 
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                return true;
            } else {
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string)) return false;
            }
            
        default:
            return false;
    }

    // 对于非 FILE_REQUEST 的情况 (比如刚才的 JSON，或者错误码)
    // 我们只需要发送 m_write_buf 这一块内存
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

void HttpConn::process() {
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT); 
}