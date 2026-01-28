#include "http_conn.h"
#include "sql_conn_pool.h" 
#include <sys/epoll.h>
#include <stdarg.h> 
#include <map>  
#include <iostream>
#include "log.h" // 【新增】引入日志

using namespace std;

const char* doc_root = "resources";

int HttpConn::m_epollfd = -1;
int HttpConn::m_user_count = 0;

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

void HttpConn::init(int sockfd, const sockaddr_in& addr) {
    m_sockfd = sockfd;
    m_address = addr;
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd(m_epollfd, sockfd, true); 
    m_user_count++;
    init_parse_state();
    
    // LOG_INFO("User Init, FD: %d, IP: %s", sockfd, inet_ntoa(addr.sin_addr));
}

void HttpConn::init_parse_state() {
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_method = GET;
    m_read_idx = 0;
    m_checked_idx = 0;
    m_start_line = 0;
    m_write_idx = 0;
    m_file_address = 0;
    m_content_length = 0; 
    m_string = nullptr;   
    
    memset(m_read_buf, 0, sizeof(m_read_buf));
    memset(m_write_buf, 0, sizeof(m_write_buf));
    memset(m_real_file, 0, sizeof(m_real_file));
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
    char* url = strpbrk(text, " \t");
    if (!url) return BAD_REQUEST;
    *url++ = '\0';
    char* method = text;
    
    if (strcasecmp(method, "GET") == 0) m_method = GET;
    else if (strcasecmp(method, "POST") == 0) m_method = POST; 
    else return BAD_REQUEST;
    
    url += strspn(url, " \t");
    char* version = strpbrk(url, " \t");
    if (!version) return BAD_REQUEST;
    *version++ = '\0';
    if (strcasecmp(version, "HTTP/1.1") != 0) return BAD_REQUEST;
    if (strncasecmp(url, "http://", 7) == 0) {
        url += 7;
        url = strchr(url, '/');
    }
    if (!url || url[0] != '/') return BAD_REQUEST;
    
    if (strlen(url) == 1) strcat(url, "index.html");
    
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, url, sizeof(m_real_file) - len - 1);
    
    // LOG_INFO("Request URL: %s", url); // 记录请求路径
    
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
    else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    return NO_REQUEST;
}

HttpConn::HTTP_CODE HttpConn::parse_content(char* text) {
    if (m_read_idx >= (m_checked_idx + m_content_length)) {
        text[m_content_length] = '\0'; 
        m_string = text; 
        return GET_REQUEST;
    }
    return NO_REQUEST;
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
    const char *p = strrchr(m_real_file, '/');
    if (!p) return BAD_REQUEST;
    char action = *(p + 1);

    char m_url[200] = {0}; 
    int len = strlen(doc_root);

    if (m_method == POST && action == '5') {
         strcpy(m_url, "/register.html");
    }
    else if (m_method == POST && (action == '1' || action == '6')) {
        char name[100] = {0};
        char password[100] = {0};
        
        if(m_string != nullptr && m_content_length > 0) {
            int i;
            for(i = 5; i < m_content_length && m_string[i] != '&' && (i-5) < 99; ++i)
                name[i - 5] = m_string[i];
            name[i - 5] = '\0';

            int j = 0;
            for(i = i + 10; i < m_content_length && m_string[i] != '\0' && j < 99; ++i, ++j)
                password[j] = m_string[i];
            password[j] = '\0';
        }

        // 记录登录/注册请求日志
        LOG_INFO("POST Request - Action:%c, User:%s", action, name);

        MYSQL *mysql = NULL;
        SqlConnRAII mysql_guard(&mysql, SqlConnPool::Instance()); 
        
        if (mysql == NULL) {
            LOG_ERROR("DB Connection Fail");
            strcpy(m_url, "/log_error.html");
        } 
        else {
            if(action == '1') { // 登录
                char query[256] = {0};
                sprintf(query, "SELECT username, password FROM user WHERE username='%s' LIMIT 1", name);
                
                if(mysql_query(mysql, query)) {
                    LOG_ERROR("SQL Error: %s", query);
                    strcpy(m_url, "/log_error.html"); 
                } else {
                    MYSQL_RES *result = mysql_store_result(mysql);
                    if(result && mysql_num_rows(result) > 0) {
                        MYSQL_ROW row = mysql_fetch_row(result);
                        if(row[1] && strcmp(password, row[1]) == 0) {
                            LOG_INFO("Login Success: %s", name);
                            strcpy(m_url, "/welcome.html"); 
                        } else {
                            LOG_INFO("Login Fail (Pass Error): %s", name);
                            strcpy(m_url, "/log_error.html"); 
                        }
                    } else {
                        LOG_INFO("Login Fail (User Not Found): %s", name);
                        strcpy(m_url, "/log_error.html"); 
                    }
                    if(result) mysql_free_result(result);
                }
            }
            else if(action == '6') { // 注册
                char query[256] = {0};
                sprintf(query, "SELECT username FROM user WHERE username='%s' LIMIT 1", name);
                
                if(mysql_query(mysql, query)) {
                    LOG_ERROR("SQL Error: %s", query);
                    strcpy(m_url, "/log_error.html");
                } else {
                    MYSQL_RES *result = mysql_store_result(mysql);
                    if(result && mysql_num_rows(result) > 0) {
                        LOG_WARN("Register Fail (User Exists): %s", name);
                        strcpy(m_url, "/log_error.html"); 
                    } else {
                        char insert_sql[256] = {0};
                        sprintf(insert_sql, "INSERT INTO user(username, password) VALUES('%s', '%s')", name, password);
                        
                        if(mysql_query(mysql, insert_sql)) {
                            LOG_ERROR("SQL Insert Error: %s", insert_sql);
                            strcpy(m_url, "/log_error.html");
                        } else {
                            LOG_INFO("Register Success: %s", name);
                            strcpy(m_url, "/welcome.html"); 
                        }
                    }
                    if(result) mysql_free_result(result);
                }
            }
        }
    }

    if(strlen(m_url) > 0) {
        strcpy(m_real_file, doc_root);
        strncpy(m_real_file + len, m_url, strlen(m_url));
    }

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
            if (true) { 
                init_parse_state();
                return true;
            } else {
                return false;
            }
        }
    }
}

bool HttpConn::add_response(const char* format, ...) {
    if (m_write_idx >= sizeof(m_write_buf)) return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, sizeof(m_write_buf) - 1 - m_write_idx, format, arg_list);
    if (len >= (sizeof(m_write_buf) - 1 - m_write_idx)) return false;
    m_write_idx += len;
    va_end(arg_list);
    return true;
}
bool HttpConn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool HttpConn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_blank_line();
    return true;
}
bool HttpConn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}
bool HttpConn::add_content_type() {
    return add_response("Content-Type: text/html\r\n");
}
bool HttpConn::add_blank_line() {
    return add_response("\r\n");
}

bool HttpConn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR:
            add_status_line(500, "Internal Server Error");
            break;
        case NO_RESOURCE:
            add_status_line(404, "Not Found");
            break;
        case FORBIDDEN_REQUEST:
            add_status_line(403, "Forbidden");
            break;
        case FILE_REQUEST:
            add_status_line(200, "OK");
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address; 
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        default:
            return false;
    }
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
    write(); 
}