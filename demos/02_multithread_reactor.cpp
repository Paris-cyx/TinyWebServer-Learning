#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <thread> // 核心：引入多线程库

using namespace std;

// ============================================
// 子线程的工作函数：专门负责和一个客户端聊天
// ============================================
void handle_client(int client_socket) {
    cout << "[子线程] ID:" << this_thread::get_id() << " 正在服务客户端..." << endl;

    char buffer[1024] = {0};
    
    // 循环接收数据，直到客户端挂断
    while (true) {
        memset(buffer, 0, sizeof(buffer)); // 清空缓存
        int valread = read(client_socket, buffer, 1024);
        
        if (valread <= 0) {
            // 如果读取到的字节数 <= 0，说明客户端断开连接或者出错了
            cout << "[子线程] 客户端已断开。" << endl;
            break;
        }
        
        cout << "[收到消息] " << buffer << endl;
        
        // 给客户端回一条消息 (Echo)
        string reply = "Server 已收到: " + string(buffer);
        send(client_socket, reply.c_str(), reply.length(), 0);
    }

    // 服务结束，挂断电话
    close(client_socket);
    cout << "[子线程] 服务结束，资源释放。" << endl;
}

int main() {
    // 1. 创建 Socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) return -1;

    // 2. 绑定 IP 端口
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);

    // 端口复用 (小技巧：防止重启服务器时报 "Address already in use")
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        cerr << "Bind 失败" << endl;
        return -1;
    }

    // 3. 监听
    if (listen(server_fd, 5) < 0) { // 允许排队 5 人
        cerr << "Listen 失败" << endl;
        return -1;
    }
    cout << "=== 多线程 Server 启动，正在监听 8080 ===" << endl;

    // 4. 主循环：只负责接客！
    while (true) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // 主线程在这里阻塞，等待新连接
        int new_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (new_socket < 0) {
            cerr << "Accept 失败" << endl;
            continue; // 接客失败没关系，继续等下一个
        }

        cout << ">>> 主线程: 接到一个新连接！马上创建子线程去处理..." << endl;

        // ============================================
        // 关键点：创建新线程
        // ============================================
        // 参数1: 线程要执行的函数 (handle_client)
        // 参数2: 传递给函数的参数 (new_socket)
        thread t(handle_client, new_socket);
        
        // detach(): 让子线程“自生自灭”，主线程不管它了，继续回去 accept
        // 如果不 detach，主线程就得等着子线程结束才能继续，那就又变成单线程了
        t.detach(); 
    }

    close(server_fd);
    return 0;
}