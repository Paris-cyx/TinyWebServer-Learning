#include <iostream>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include "ThreadPool.h" // 引入刚才写的线程池

using namespace std;

const int MAX_EVENTS = 1000;

// 设置非阻塞模式（大厂标准写法，防止 read 卡死）
// 这里先简单略过，面试常考 fcntl

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        return -1;
    }
    listen(server_fd, 10);

    // ============================================
    // 1. 启动 4 个工人的线程池
    // ============================================
    ThreadPool pool(4);
    cout << "=== Reactor Server (Epoll + ThreadPool) 启动 ===" << endl;

    int epoll_fd = epoll_create1(0);
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    struct epoll_event events[MAX_EVENTS];

    while (true) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n; i++) {
            int curr_fd = events[i].data.fd;

            if (curr_fd == server_fd) {
                // 新连接：主线程自己处理（因为很快）
                sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                
                event.events = EPOLLIN;
                event.data.fd = client_socket;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &event);
                cout << ">>> 新连接 FD: " << client_socket << endl;
            } 
            else {
                // 有数据来了！
                // 1. 主线程负责把数据读出来（IO 操作）
                char buffer[1024] = {0};
                int valread = read(curr_fd, buffer, 1024);

                if (valread <= 0) {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, nullptr);
                    close(curr_fd);
                    cout << "客户端断开" << endl;
                } 
                else {
                    // 2. 只有读到了数据，才把“处理任务”扔给线程池
                    // 使用 Lambda 表达式把 buffer 和 curr_fd 捕获进去
                    string req(buffer); // 转成 string 安全传递
                    
                    pool.enqueue([req, curr_fd] {
                        // --- 这里是子线程在执行 ---
                        // 模拟耗时业务逻辑 (比如查数据库需要 2秒)
                        // this_thread::sleep_for(chrono::seconds(2)); 
                        
                        cout << "[子线程 " << this_thread::get_id() << "] 处理请求: " << req << endl;
                        
                        string reply = "Processed by Worker: " + req;
                        send(curr_fd, reply.c_str(), reply.length(), 0);
                    });
                }
            }
        }
    }
    close(server_fd);
    return 0;
}