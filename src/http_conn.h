#ifndef HTTPCONN_H
#define HTTPCONN_H

#include <sys/uio.h>     // writev
#include <arpa/inet.h>   // sockaddr_in
#include <sys/stat.h>    // stat
#include <fcntl.h>       // open
#include <unistd.h>      // close, write
#include <sys/mman.h>    // mmap
#include <string.h>      // memset, strcpy
#include <string>
#include <iostream>
#include <vector>
#include <map>           
#include <sys/epoll.h>   // epoll_event
#include "sql_conn_pool.h" // 数据库连接池

using namespace std;

// 全局函数声明
void setnonblocking(int fd);
void addfd(int epollfd, int fd, bool one_shot);
void removefd(int epollfd, int fd);
void modfd(int epollfd, int fd, int ev);

class HttpConn {
public:
    static const int FILENAME_LEN = 200;       
    static const int READ_BUFFER_SIZE = 1048576;  
    static const int WRITE_BUFFER_SIZE = 1024; 

    enum METHOD { GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATCH };

    enum CHECK_STATE {
        CHECK_STATE_REQUESTLINE = 0, 
        CHECK_STATE_HEADER,          
        CHECK_STATE_CONTENT          
    };

    enum LINE_STATUS {
        LINE_OK = 0,  
        LINE_BAD,     
        LINE_OPEN     
    };

    enum HTTP_CODE {
        NO_REQUEST,        
        GET_REQUEST,       
        BAD_REQUEST,       
        NO_RESOURCE,       
        FORBIDDEN_REQUEST, 
        FILE_REQUEST,      
        INTERNAL_ERROR,    
        CLOSED_CONNECTION  
    };

public:
    HttpConn() {}
    ~HttpConn() {}

    void init(int sockfd, const sockaddr_in& addr);
    void close_conn();
    void process();
    bool read_once();
    bool write();

    // 初始化数据库读取表
    void initmysql_result(SqlConnPool* connPool);

public:
    static int m_epollfd;
    static int m_user_count;

private:
    // 【新增】文件上传相关变量
    char m_boundary[100];      // 存储分界线 (比如: ------WebKitFormBoundary...)
    bool m_is_multipart;       // 标记本次请求是不是文件上传
    char m_upload_filename[100]; // 存储上传的文件名 (比如: avatar.jpg)
    char* m_file_content;      // 指向 POST 请求体中文件数据的起始位置

    // 【新增】专门解析 multipart 协议的函数
    HTTP_CODE parse_multipart_content(char* text);

    // 【新增】Cookie 业务相关变量
    int m_set_cookie;        // 标记是否需要在响应头设置 Set-Cookie
    bool m_cookie_is_login;
    
    int m_sockfd;
    sockaddr_in m_address;

    char m_read_buf[READ_BUFFER_SIZE];
    int m_read_idx;
    int m_checked_idx;
    int m_start_line;

    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;

    CHECK_STATE m_check_state;
    METHOD m_method;

    char* m_real_file;    
    char* m_url;          
    char* m_version;      
    char* m_host;         
    int m_content_length; 
    bool m_linger;        

    char* m_file_address; 
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;

    char* m_string;       
    
    char m_sql_user[100];
    char m_sql_passwd[100];

    // 【核心修复】之前漏掉了这个声明，导致报错
    void init_parse_state(); 
    
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);

    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text); 
    HTTP_CODE do_request();
    
    char* get_line() { return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();
    void unmap();
    
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

    bool m_is_json;         // 标记本次响应是否为 JSON
    char* m_json_string;    // 存储要发送的 JSON 字符串内容
};

#endif