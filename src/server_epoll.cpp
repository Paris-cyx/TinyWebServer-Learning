#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <signal.h> 
#include "ThreadPool.h"
#include "http_conn.h" 
#include "sql_conn_pool.h" 
#include "log.h"  // 【新增】引入日志头文件

const int MAX_EVENTS = 10000;
const int MAX_FD = 65536; 

int main() {
    // 1. 初始化日志系统 (异步模式，队列长度800)
    // 这里的 "ServerLog" 是日志文件名的前缀
    Log::Instance()->init("ServerLog", 1, 2000, 800000, 800); 
    
    LOG_INFO("========== Server Start ==========");
    LOG_INFO("Log System Init Success, Async Mode Enabled");

    // 2. 忽略 SIGPIPE
    signal(SIGPIPE, SIG_IGN); 

    // 3. 初始化数据库连接池
    SqlConnPool::Instance()->init("localhost", 3306, "tiny", "123456", "webserver", 8);
    LOG_INFO("SQL Pool Init Success, Conn Size: %d", 8);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);
    
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERROR("Bind Error: %d", errno); // 【新增】记录错误日志
        return -1;
    }
    
    listen(server_fd, 10000); 

    int epoll_fd = epoll_create1(0);
    HttpConn::m_epollfd = epoll_fd;

    struct epoll_event event;
    event.data.fd = server_fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);
    
    int old_option = fcntl(server_fd, F_GETFL);
    fcntl(server_fd, F_SETFL, old_option | O_NONBLOCK);

    ThreadPool pool(4); 
    HttpConn* users = new HttpConn[MAX_FD];
    struct epoll_event events[MAX_EVENTS];

    LOG_INFO("Server Init Finish, Listening on 8080...");

    while (true) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n; i++) {
            int sockfd = events[i].data.fd;

            if (sockfd == server_fd) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int connfd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                
                if (connfd < 0) {
                    LOG_ERROR("Accept Error: %d", errno);
                    continue;
                }
                if (HttpConn::m_user_count >= MAX_FD) {
                    LOG_WARN("Server Busy, User Count: %d", HttpConn::m_user_count);
                    close(connfd);
                    continue;
                }
                users[connfd].init(connfd, client_addr);
                // LOG_INFO("New Client Connected, FD: %d", connfd); // 可选：记录新连接
            }
            else if (events[i].events & EPOLLIN) {
                if (users[sockfd].read_once()) {
                    pool.enqueue([&users, sockfd] {
                        users[sockfd].process();
                    });
                } else {
                    users[sockfd].close_conn();
                }
            }
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