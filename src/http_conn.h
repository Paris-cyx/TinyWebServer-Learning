#ifndef HTTPCONN_H
#define HTTPCONN_H

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

private:
    // --- 私有解析方法 (状态机逻辑) ---
    void init_parse_state();                        // 重置解析状态
    HTTP_CODE process_read();                       // 主状态机驱动
    HTTP_CODE parse_request_line(char* text);       // 解析请求行
    HTTP_CODE parse_headers(char* text);            // 解析头部
    HTTP_CODE parse_content(char* text);            // 解析内容
    LINE_STATUS parse_line();                       // 从状态机：读取一行
    
    // --- 私有响应方法 ---
    bool process_write(HTTP_CODE ret);              // 根据结果生成响应
    // ... 这里为了简化，我们暂时只列出核心

public:
    // 静态成员：所有 socket 共享同一个 epoll 实例
    static int m_epollfd;
    static int m_user_count; // 统计用户数

private:
    int m_sockfd;
    sockaddr_in m_address;

    // 读缓冲区
    char m_read_buf[2048];   // 读缓冲区
    int m_read_idx;          // 缓冲区中数据的下一个写入位置
    int m_checked_idx;       // 当前分析到的位置
    int m_start_line;        // 当前行的起始位置

    // 解析相关
    CHECK_STATE m_check_state; // 主状态机当前状态
    METHOD m_method;           // 请求方法

    // 响应相关 (文件信息)
    char m_real_file[200];   // 文件的绝对路径
    struct stat m_file_stat; // 文件属性
    char* m_file_address;    // mmap 映射后的内存地址 (零拷贝)
};

#endif