#ifndef HTTPCONN_H
#define HTTPCONN_H

#include <sys/uio.h>     // 提供 writev 函数
#include <arpa/inet.h>   // sockaddr_in
#include <sys/stat.h>    // stat
#include <fcntl.h>       // open
#include <unistd.h>      // close, write
#include <sys/mman.h>    // mmap (零拷贝关键)
#include <string.h>      // memset, strcpy
#include <string>
#include <iostream>

using namespace std;

void setnonblocking(int fd);

class HttpConn {
public:
    // HTTP 请求方法（目前我们只实现 GET）
    enum METHOD { GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATCH };

    // =======================================================
    // 核心：主状态机的三种状态
    // =======================================================
    enum CHECK_STATE {
        CHECK_STATE_REQUESTLINE = 0, // 当前正在分析请求行 (GET / HTTP/1.1)
        CHECK_STATE_HEADER,          // 当前正在分析头部字段 (Host: localhost)
        CHECK_STATE_CONTENT          // 当前正在分析请求体 (POST的数据)
    };

    // =======================================================
    // 核心：从状态机的三种状态 (用于解析每一行)
    // =======================================================
    enum LINE_STATUS {
        LINE_OK = 0,  // 完整读取了一行
        LINE_BAD,     // 这行语法错误
        LINE_OPEN     // 这行数据不完整 (半包)，需要继续接收数据
    };

    // HTTP 处理结果
    enum HTTP_CODE {
        NO_REQUEST,        // 请求不完整，需要继续读取
        GET_REQUEST,       // 获得了完整的 GET 请求
        BAD_REQUEST,       // 语法错误
        NO_RESOURCE,       // 404 没资源
        FORBIDDEN_REQUEST, // 403 没权限
        FILE_REQUEST,      // 200 正常请求文件
        INTERNAL_ERROR,    // 500 服务器内部错误
        CLOSED_CONNECTION  // 客户端断开
    };

public:
    HttpConn() {}
    ~HttpConn() {}

    // 初始化连接 (保存 socket 和地址)
    void init(int sockfd, const sockaddr_in& addr);

    // 关闭连接
    void close_conn();

    // =========================================
    // 核心业务接口
    // =========================================
    // 1. 读数据 (一次性读完所有数据 - ET模式)
    bool read_once();

    // 2. 处理请求 (整个解析 + 响应的入口)
    void process();

    // 3. 写数据 (响应发送给客户端)
    bool write();

public:
    // 静态成员：所有 socket 共享同一个 epoll 实例
    static int m_epollfd;
    static int m_user_count; // 统计用户数

private:
    int m_sockfd;
    sockaddr_in m_address;

    char m_read_buf[2048];
    int m_read_idx;
    int m_checked_idx;
    int m_start_line;

    // 【新增】写缓冲区 (用于存放 HTTP 头部)
    char m_write_buf[1024];
    int m_write_idx;

    CHECK_STATE m_check_state;
    METHOD m_method;

    // 【新增】文件处理相关
    char m_real_file[200];   // 文件的绝对路径
    struct stat m_file_stat; // 文件状态信息
    char* m_file_address;    // mmap 映射后的内存地址
    
    // 【新增】writev 相关 (分散写)
    struct iovec m_iv[2];    // io向量：m_iv[0]放头部，m_iv[1]放文件内容
    int m_iv_count;          // 有几块内存要写

    // --- 私有方法 ---
    void init_parse_state();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);

    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text);
    
    HTTP_CODE do_request(); // 【核心】分析请求的文件
    
    LINE_STATUS parse_line();
    void unmap(); // 【核心】解除内存映射
    
    // 【新增】辅助函数：往写缓冲里填数据
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
};

#endif