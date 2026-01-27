#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include "ThreadPool.h"
#include "http_conn.h" // 引入我们写好的 HTTP 类

using namespace std;

const int MAX_EVENTS = 10000;
const int MAX_FD = 65536; // 最大文件描述符数量

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    // 优雅关闭：允许端口复用
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);
    
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        return -1;
    }
    listen(server_fd, 10000);

    // 创建 epoll 对象
    int epoll_fd = epoll_create1(0);
    
    // 把 epoll_fd 传给 HttpConn 类 (静态成员，所有对象共享)
    HttpConn::m_epollfd = epoll_fd;

    // 监听 server_fd
    struct epoll_event event;
    event.data.fd = server_fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);
    setnonblocking(server_fd); // 监听 socket 也要非阻塞

    // 创建线程池
    ThreadPool pool(4);
    
    // 预先为每个可能的客户端分配一个 HttpConn 对象
    // 这样避免频繁 new/delete，直接用 fd 作为数组下标索引
    HttpConn* users = new HttpConn[MAX_FD];

    struct epoll_event events[MAX_EVENTS];

    //cout << "=== TinyWebServer v1.0 启动成功 ===" << endl;

    while (true) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n; i++) {
            int sockfd = events[i].data.fd;

            // 1. 新连接到来
            if (sockfd == server_fd) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int connfd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                
                if (connfd < 0) {
                    cout << "Accept Error" << endl;
                    continue;
                }
                if (HttpConn::m_user_count >= MAX_FD) {
                    cout << "服务器正忙..." << endl;
                    close(connfd);
                    continue;
                }
                
                // 直接初始化这个用户的连接 (放入 epoll 监控都在这里面做了)
                users[connfd].init(connfd, client_addr);
            }
            // 2. 读事件 (EPOLLIN)
            else if (events[i].events & EPOLLIN) {
                // 一次性把数据读完
                if (users[sockfd].read_once()) {
                    // 读取成功，把“业务逻辑”扔给线程池
                    // 这里的 Lambda 只需要传指针，非常快
                    pool.enqueue([&users, sockfd] {
                        users[sockfd].process();
                    });
                } else {
                    // 读失败（对方关闭连接），移除
                    users[sockfd].close_conn();
                }
            }
            // 3. 错误事件
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                users[sockfd].close_conn();
            }
        }
    }
    
    delete[] users;
    close(epoll_fd);
    close(server_fd);
    return 0;
}