#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <signal.h>
#include <assert.h>
#include <errno.h>
#include "ThreadPool.h"
#include "http_conn.h"
#include "sql_conn_pool.h"
#include "log.h"
#include "lst_timer.h"

const int MAX_EVENTS = 10000;
const int MAX_FD = 1000;//这是为了测试文件上传功能，webbench压力测试时请改回65536
const int TIMESLOT = 5; // 最小超时单位：5秒

static int pipefd[2];           // 管道：0读，1写
static time_heap timer_lst(10000);//初始化最小堆，容量 10000
static int epoll_fd = 0;
static HttpConn* users = nullptr; // 全局指针

// 信号处理函数
void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0); // 往管道写信号
    errno = save_errno;
}

// 设置信号函数
void addsig(int sig) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

// 定时器回调函数：删除非活动连接
void cb_func(client_data* user_data) {
    if (!user_data) return;
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    close(user_data->sockfd);
    HttpConn::m_user_count--;
    LOG_INFO("Kick Client (Timeout): fd=%d", user_data->sockfd);
}

int main() {
    // 1. 初始化日志 (开启全量日志模式)
    Log::Instance()->init("ServerLog", 0, 2000, 800000, 800);
    
    // 2. 初始化数据库
    SqlConnPool::Instance()->init("localhost", 3306, "tiny", "123456", "webserver", 8);

    // 3. 忽略 SIGPIPE
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);
    
    bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_fd, 10000);

    epoll_fd = epoll_create1(0);
    HttpConn::m_epollfd = epoll_fd;

    // 添加 server_fd 到 epoll (函数定义已在 http_conn.cpp 中)
    addfd(epoll_fd, server_fd, false);
    
    // 创建管道
    socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    setnonblocking(pipefd[1]); // 写端非阻塞
    addfd(epoll_fd, pipefd[0], false); // 读端加入 epoll

    // 设置信号
    addsig(SIGALRM); // 定时信号
    addsig(SIGTERM); // kill 信号
    addsig(SIGINT);  // Ctrl+C 信号 (用于优雅退出检测内存)
    alarm(TIMESLOT); // 开启闹钟

    ThreadPool pool(4);
    users = new HttpConn[MAX_FD];
    users->initmysql_result(SqlConnPool::Instance());
    client_data *users_timer = new client_data[MAX_FD];

    struct epoll_event events[MAX_EVENTS];
    bool timeout = false;
    bool stop_server = false;

    LOG_INFO("Server Start with Timer System...");

    while (!stop_server) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        
        if (n < 0 && errno != EINTR) {
            LOG_ERROR("Epoll Failure");
            break;
        }

        for (int i = 0; i < n; i++) {
            int sockfd = events[i].data.fd;

            // 1. 新连接
            if (sockfd == server_fd) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int connfd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                
                if (connfd < 0) continue;
                if (HttpConn::m_user_count >= MAX_FD) {
                    close(connfd);
                    continue;
                }
                
                users[connfd].init(connfd, client_addr);

                // 绑定定时器
                users_timer[connfd].address = client_addr;
                users_timer[connfd].sockfd = connfd;
                
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT; // 15s 后过期
                
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
            }
            // 2. 处理信号 (管道读端)
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                char signals[1024];
                int ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret <= 0) continue;
                else {
                    for (int i = 0; i < ret; ++i) {
                        switch (signals[i]) {
                            case SIGALRM:
                                timeout = true;
                                break;
                            case SIGTERM:
                            case SIGINT: // 处理 Ctrl+C
                                stop_server = true;
                                break;
                        }
                    }
                }
            }
            // 3. 读事件
            else if (events[i].events & EPOLLIN) {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].read_once()) {
                    if (timer) {
                        timer_lst.adjust_timer(timer); 
                    }
                    
                    pool.enqueue([sockfd] {
                        users[sockfd].process();
                    });
                } else {
                    // 读失败，关闭连接
                    if (timer) timer_lst.del_timer(timer);
                    users[sockfd].close_conn();
                }
            }
            // 4. 写事件
            else if (events[i].events & EPOLLOUT) {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write()) {
                    if (timer) {
                        timer_lst.adjust_timer(timer);
                    }
                } else {
                    if (timer) timer_lst.del_timer(timer);
                    users[sockfd].close_conn();
                }
            }
            // 5. 异常
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                util_timer *timer = users_timer[sockfd].timer;
                if (timer) timer_lst.del_timer(timer);
                users[sockfd].close_conn();
            }
        }

        if (timeout) {
            timer_lst.tick();
            alarm(TIMESLOT);
            timeout = false;
        }
    }
    
    // 优雅退出后的资源清理
    close(epoll_fd);
    close(server_fd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    return 0;
}