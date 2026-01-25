#include <iostream>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h> // 核心：Epoll 头文件

using namespace std;

const int MAX_EVENTS = 1000; // 一次最多处理多少个事件

int main() {
    // 1. 创建监听 Socket (和之前一样)
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);
    
    // 端口复用
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        return -1;
    }
    listen(server_fd, 10);
    cout << "=== Epoll Server 启动，监听 8080 ===" << endl;

    // ============================================
    // 2. 创建 Epoll 实例 (买一个“电子大屏”)
    // ============================================
    // 参数 0 是由系统自动选择大小，不用管
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("Epoll create failed");
        return -1;
    }

    // ============================================
    // 3. 把“前台电话” (server_fd) 贴上标签，放进大屏监控
    // ============================================
    struct epoll_event event;
    event.events = EPOLLIN; // EPOLLIN 表示：关注“有数据读”的事件
    event.data.fd = server_fd; // 关联的文件描述符

    // epoll_ctl: 控制函数 (CTL = Control)
    // EPOLL_CTL_ADD: 添加监控
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        perror("Epoll add failed");
        return -1;
    }

    // 准备一个数组，用来接收发生的事件
    struct epoll_event events[MAX_EVENTS];

    // ============================================
    // 4. 主循环：盯着大屏看
    // ============================================
    while (true) {
        // epoll_wait: 等待事件发生 (阻塞)
        // 返回值 n: 有 n 个事件发生了 (比如有 3 个人说话了)
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        // 遍历这 n 个发生的事件
        for (int i = 0; i < n; i++) {
            int curr_fd = events[i].data.fd;

            if (curr_fd == server_fd) {
                // Situation A: 前台电话响了 (有新连接)
                sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                
                cout << ">>> 新客户端连接: " << client_socket << endl;

                // 把这个新客户端也加到 Epoll 监控列表里
                event.events = EPOLLIN; // 依然只关注“读”
                event.data.fd = client_socket;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &event);
            } 
            else {
                // Situation B: 之前的某个客户端说话了
                char buffer[1024] = {0};
                int valread = read(curr_fd, buffer, 1024);

                if (valread <= 0) {
                    // 对方挂断了，或者出错了
                    cout << "客户端 " << curr_fd << " 断开连接。" << endl;
                    
                    // 从 Epoll 中移除 (Linux 会自动移除关闭的 fd，但手动移除是好习惯)
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, nullptr);
                    close(curr_fd);
                } 
                else {
                    // 正常收到消息
                    cout << "[收到消息 FD:" << curr_fd << "] " << buffer << endl;
                    
                    // 回复消息
                    string reply = "Epoll Server got: " + string(buffer);
                    send(curr_fd, reply.c_str(), reply.length(), 0);
                }
            }
        }
    }

    close(server_fd);
    close(epoll_fd);
    return 0;
}