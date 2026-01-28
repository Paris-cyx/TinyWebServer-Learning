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
#include "lst_timer.h" // 【新增】引入定时器头文件

const int MAX_EVENTS = 10000;
const int MAX_FD = 65536; 
const int TIMESLOT = 5; // 【定时器】最小超时单位：5秒

static int pipefd[2];           // 【定时器】管道：0读，1写
static sort_timer_lst timer_lst;// 【定时器】升序链表
static int epoll_fd = 0;
static HttpConn* users = nullptr; // 把users变成全局，方便回调函数使用

// 【定时器】信号处理函数：仅仅是通过管道发送信号值，不处理业务
void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0); // 往管道里写一个字节
    errno = save_errno;
}

// 【定时器】设置信号函数
void addsig(int sig) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

// 【定时器】回调函数：当用户超时，删除非活动连接
void cb_func(client_data* user_data) {
    if (!user_data) return;
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    close(user_data->sockfd);
    HttpConn::m_user_count--;
    LOG_INFO("Kick Client (Timeout): fd=%d", user_data->sockfd);
}

int main() {
    // 1. 初始化日志 (这里关掉了日志输出以提高性能，如果想看踢人过程，把第二个参数1改回0)
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

    addfd(epoll_fd, server_fd, false);
    
    // 【定时器】创建管道
    socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    setnonblocking(pipefd[1]); // 写端非阻塞
    addfd(epoll_fd, pipefd[0], false); // 监听管道读端

    // 【定时器】设置信号处理
    addsig(SIGALRM); // 定时信号
    addsig(SIGTERM); // 终止信号
    alarm(TIMESLOT); // 开启闹钟：5秒后响第一次

    ThreadPool pool(4); 
    users = new HttpConn[MAX_FD];
    client_data *users_timer = new client_data[MAX_FD]; // 存定时器数据

    struct epoll_event events[MAX_EVENTS];
    bool timeout = false;
    bool stop_server = false;

    LOG_INFO("Server Start with Timer System...");

    while (!stop_server) {
        // epoll_wait 等待事件
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        
        if (n < 0 && errno != EINTR) {
            LOG_ERROR("Epoll Failure");
            break;
        }

        for (int i = 0; i < n; i++) {
            int sockfd = events[i].data.fd;

            // 1. 处理新连接
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

                // 【定时器】给新用户绑定定时器
                users_timer[connfd].address = client_addr;
                users_timer[connfd].sockfd = connfd;
                
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT; // 设为 15秒 (3*5) 后过期
                
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer); // 加入链表
            }
            // 2. 【定时器】处理信号（管道读端有数据）
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                char signals[1024];
                // 从管道把信号读出来，清空管道
                int ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1) continue;
                else if (ret == 0) continue;
                else {
                    for (int i = 0; i < ret; ++i) {
                        switch (signals[i]) {
                            case SIGALRM:
                                timeout = true; // 标记：该去处理定时任务了
                                break;
                            case SIGTERM:
                                stop_server = true;
                                break;
                        }
                    }
                }
            }
            // 3. 处理客户端读请求
            else if (events[i].events & EPOLLIN) {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].read_once()) {
                    // 【定时器】如果用户发数据了，说明他活着，重置他的死亡时间
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("Adjust Timer for fd %d", sockfd);
                        timer_lst.adjust_timer(timer);
                    }
                    pool.enqueue([&users, sockfd] {
                        users[sockfd].process();
                    });
                } else {
                    // 读失败（对方关闭连接），直接移除定时器并关闭
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                    users[sockfd].close_conn();
                }
            }
            // 4. 处理客户端写请求
            else if (events[i].events & EPOLLOUT) {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write()) {
                    // 【定时器】只要有数据传输，就给他续命
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                } else {
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                    users[sockfd].close_conn();
                }
            }
            // 5. 异常处理
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                util_timer *timer = users_timer[sockfd].timer;
                if (timer) {
                    timer_lst.del_timer(timer);
                }
                users[sockfd].close_conn();
            }
        }

        // 【定时器】最后处理定时任务
        // 为什么放在最后？因为 IO 事件优先级更高，定时任务晚个几毫秒没关系
        if (timeout) {
            timer_lst.tick(); // 巡逻链表，踢掉过期用户
            alarm(TIMESLOT);  // 重置闹钟，5秒后再响
            timeout = false;
        }
    }
    
    close(epoll_fd);
    close(server_fd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    return 0;
}