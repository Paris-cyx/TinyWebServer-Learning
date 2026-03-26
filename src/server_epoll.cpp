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

static int pipefd[2];           // 管道：0读，1写（统一事件源，用于信号→主循环通知）
static time_heap timer_lst(10000);//初始化最小堆，容量 10000
static int epoll_fd = 0;
static HttpConn* users = nullptr; // 全局连接对象数组，下标即 fd

// 【统一事件源】信号处理函数：只做一件事——把信号编号写入管道
// 这样可以在主循环中通过 epoll_wait 统一处理信号，避免在异步信号上下文中
// 调用不安全函数（如 printf、malloc 等），从根本上避免死锁与数据竞争。
void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0); // 往管道写端写入信号编号（非阻塞）
    errno = save_errno;
}

// 注册信号处理函数，SA_RESTART 让被信号打断的系统调用自动重启
void addsig(int sig) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

// 定时器超时回调：从 epoll 注销并关闭超时连接
// 由 time_heap::tick() 在主循环中调用，确保在主线程安全执行，无需加锁
void cb_func(client_data* user_data) {
    if (!user_data) return;
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, user_data->sockfd, 0); // 从 epoll 注销 fd
    close(user_data->sockfd);                                  // 关闭套接字
    HttpConn::m_user_count--;
    LOG_INFO("Kick Client (Timeout): fd=%d", user_data->sockfd);
}

int main() {
    // =========================================================
    // 【阶段 1】基础设施初始化
    // 详细调用链路见 docs/reactor-call-chain.md
    // =========================================================

    // 1. 初始化日志 (开启全量日志模式)
    Log::Instance()->init("./log/ServerLog", 0, 2000, 800000, 800);
    
    // 2. 初始化数据库连接池，并缓存用户表到内存 map
    SqlConnPool::Instance()->init("localhost", 3306, "tiny", "123456", "webserver", 8);

    // 3. 忽略 SIGPIPE：写端关闭时不终止进程，改为让 write/send 返回 EPIPE 错误
    signal(SIGPIPE, SIG_IGN);

    // =========================================================
    // 【阶段 2】Socket 初始化：socket → bind → listen
    // =========================================================
    int server_fd = socket(AF_INET, SOCK_STREAM, 0); // 创建 TCP 套接字
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // 端口复用

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);
    
    bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)); // 绑定 IP:8080
    listen(server_fd, 10000); // 进入监听状态，半连接队列上限 10000

    // =========================================================
    // 【阶段 3】Epoll 初始化：epoll_create1 → epoll_ctl(ADD)
    // =========================================================
    epoll_fd = epoll_create1(0);         // 创建 epoll 实例
    HttpConn::m_epollfd = epoll_fd;      // 所有 HttpConn 对象共享同一 epoll 实例

    // 将监听套接字加入 epoll，监听新连接到来（EPOLLIN）
    addfd(epoll_fd, server_fd, false);   // → epoll_ctl(EPOLL_CTL_ADD, server_fd, EPOLLIN|EPOLLET)
    
    // =========================================================
    // 【阶段 4】统一事件源：用管道把信号转化为 epoll 可感知的 IO 事件
    // =========================================================
    socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd); // 创建全双工管道
    setnonblocking(pipefd[1]);                    // 写端非阻塞，防止 sig_handler 阻塞
    addfd(epoll_fd, pipefd[0], false);            // 管道读端加入 epoll，主循环可感知信号

    // 注册信号：到来时 sig_handler 将信号编号写入 pipefd[1]，主循环从 pipefd[0] 读取处理
    addsig(SIGALRM); // 定时信号（每 TIMESLOT 秒触发一次，用于清理超时连接）
    addsig(SIGTERM); // kill 信号
    addsig(SIGINT);  // Ctrl+C 信号 (用于优雅退出检测内存)
    alarm(TIMESLOT); // 开启第一个 5s 闹钟滴答

    // =========================================================
    // 【阶段 5】线程池 + 连接对象预分配
    // =========================================================
    // ThreadPool 构造时创建 4 个工作线程，各自阻塞在 condition_variable::wait()
    // 等待 enqueue() 推入任务后被唤醒执行
    ThreadPool pool(4);
    users = new HttpConn[MAX_FD];                     // 预分配所有连接对象（下标 = fd）
    users->initmysql_result(SqlConnPool::Instance()); // 从 MySQL 缓存用户表
    client_data *users_timer = new client_data[MAX_FD];

    struct epoll_event events[MAX_EVENTS];
    bool timeout = false;
    bool stop_server = false;

    LOG_INFO("Server Start with Timer System...");

    // =========================================================
    // 【阶段 6】主事件循环：epoll_wait → 分发事件
    // =========================================================
    while (!stop_server) {
        // 阻塞等待，直到有 fd 就绪（新连接、数据到达、可写、信号）
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        
        if (n < 0 && errno != EINTR) {
            LOG_ERROR("Epoll Failure");
            break;
        }

        for (int i = 0; i < n; i++) {
            int sockfd = events[i].data.fd;

            // 【事件 A】新连接到达：server_fd 可读 → accept
            if (sockfd == server_fd) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                // accept 从内核完成队列中取出已完成 TCP 三次握手的连接
                int connfd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                
                if (connfd < 0) continue;
                if (HttpConn::m_user_count >= MAX_FD) {
                    close(connfd);
                    continue;
                }
                
                // HttpConn::init：将 connfd 加入 epoll（EPOLLIN|EPOLLONESHOT），设为非阻塞
                users[connfd].init(connfd, client_addr);

                // 为该连接创建定时器，15s 后无活动则触发 cb_func 关闭连接
                users_timer[connfd].address = client_addr;
                users_timer[connfd].sockfd = connfd;
                
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT; // 15s 后过期
                
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer); // O(log N) 插入最小堆
            }
            // 【事件 B】管道可读 → 信号到来（SIGALRM / SIGTERM / SIGINT）
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                char signals[1024];
                int ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret <= 0) continue;
                else {
                    for (int i = 0; i < ret; ++i) {
                        switch (signals[i]) {
                            case SIGALRM:
                                timeout = true; // 延迟处理：事件分发完后再 tick
                                break;
                            case SIGTERM:
                            case SIGINT: // 处理 Ctrl+C
                                stop_server = true;
                                break;
                        }
                    }
                }
            }
            // 【事件 C】connfd 可读 → 客户端发来数据 → recv → 投入线程池
            else if (events[i].events & EPOLLIN) {
                util_timer *timer = users_timer[sockfd].timer;
                // read_once：循环调用 recv 直到 EAGAIN，把数据读尽（ET 边沿触发要求）
                if (users[sockfd].read_once()) {
                    if (timer) {
                        timer_lst.adjust_timer(timer); // 刷新超时时间
                    }
                    // 将 process() 任务投入线程池队列，工作线程异步执行 HTTP 解析与响应构建
                    pool.enqueue([sockfd] {
                        users[sockfd].process();
                    });
                } else {
                    // 读失败（对端关闭 / 出错），清理连接
                    if (timer) timer_lst.del_timer(timer);
                    users[sockfd].close_conn();
                }
            }
            // 【事件 D】connfd 可写 → 内核发送缓冲区有空间 → writev 发送响应
            else if (events[i].events & EPOLLOUT) {
                util_timer *timer = users_timer[sockfd].timer;
                // write：调用 writev(响应头 + mmap 文件数据)，发完后 munmap + 重置或关闭
                if (users[sockfd].write()) {
                    if (timer) {
                        timer_lst.adjust_timer(timer);
                    }
                } else {
                    if (timer) timer_lst.del_timer(timer);
                    users[sockfd].close_conn();
                }
            }
            // 【事件 E】异常：对端关闭 / 挂起 / 错误 → 直接清理
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                util_timer *timer = users_timer[sockfd].timer;
                if (timer) timer_lst.del_timer(timer);
                users[sockfd].close_conn();
            }
        }

        // 所有就绪事件处理完毕后，再统一处理定时器超时（避免阻塞事件分发）
        if (timeout) {
            timer_lst.tick(); // 遍历最小堆，对所有过期连接执行 cb_func
            alarm(TIMESLOT);  // 重设下一个 5s 滴答
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