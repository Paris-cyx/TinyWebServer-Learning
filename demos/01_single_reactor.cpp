#include <iostream>
#include <sys/socket.h> // 核心：socket 函数都在这里
#include <netinet/in.h> // 核心：定义了 IP 地址结构体
#include <unistd.h>     // 核心：close 函数在这里
#include <cstring>      // memset 等工具

using namespace std;

int main() {
    // ============================================
    // 第一步：创建 Socket (买一部手机)
    // ============================================
    // AF_INET: 使用 IPv4 地址 (最常用的)
    // SOCK_STREAM: 使用 TCP 协议 (面向连接，稳定可靠)
    // 0: 自动选择默认协议
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        cerr << "Socket 创建失败！" << endl;
        return -1;
    }
    cout << "1. Socket 创建成功 (手机买好了)" << endl;

    // ============================================
    // 第二步：Bind 绑定 IP 和端口 (办张手机卡)
    // ============================================
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;           // 必须和 socket 函数里的 AF_INET 对应
    server_addr.sin_addr.s_addr = INADDR_ANY;   // 监听本机所有网卡的 IP (0.0.0.0)
    server_addr.sin_port = htons(8080);         // 端口号 8080 (htons 是为了转换字节序，防止乱码)

    // bind: 把 socket 和上面的地址绑定起来
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        cerr << "Bind 失败！端口可能被占用了。" << endl;
        return -1;
    }
    cout << "2. Bind 成功 (手机卡插好了，号码是 8080)" << endl;

    // ============================================
    // 第三步：Listen 监听 (开机，等待响铃)
    // ============================================
    // 3: 待办事项列表长度 (backlog)，即使忙不过来，也允许排队 3 个人
    if (listen(server_fd, 3) < 0) {
        cerr << "Listen 失败！" << endl;
        return -1;
    }
    cout << "3. Server 正在监听端口 8080... (等待电话打进来)" << endl;

    // ============================================
    // 第四步：Accept 接收连接 (接起电话)
    // ============================================
    // 这是一个阻塞函数！程序运行到这里会“卡住”，直到有客户端连上来
    int new_socket;
    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    new_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
    if (new_socket < 0) {
        cerr << "Accept 失败！" << endl;
        return -1;
    }
    cout << "4. 客户端连接成功！(电话接通了)" << endl;

    // ============================================
    // 第五步：读取数据 (听对方说话)
    // ============================================
    char buffer[1024] = {0}; // 准备一个缓冲区放数据
    int valread = read(new_socket, buffer, 1024);
    cout << "收到消息: " << buffer << endl;

    // ============================================
    // 第六步：关闭连接 (挂断)
    // ============================================
    close(new_socket); // 挂断当前通话
    close(server_fd);  // 关掉整个手机 (通常服务器不会跑这句，因为要一直开机)
    
    return 0;
}