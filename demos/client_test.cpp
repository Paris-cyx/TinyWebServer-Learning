#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <string>

using namespace std;

int main() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        cerr << "连接失败" << endl;
        return -1;
    }
    cout << "连接成功！请输入内容按回车发送 (输入 'exit' 退出)" << endl;

    while (true) {
        string input;
        cout << "你: ";
        getline(cin, input); // 获取键盘输入

        if (input == "exit") break;

        // 发送
        send(sock, input.c_str(), input.length(), 0);

        // 接收回信
        char buffer[1024] = {0};
        int valread = read(sock, buffer, 1024);
        if (valread > 0) {
            cout << buffer << endl;
        }
    }

    close(sock);
    return 0;
}