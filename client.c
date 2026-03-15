#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define PORT 10000
#define BUFFER_SIZE 1024
#define SERVER_IP "127.0.0.1"

int main() {
    int sock_fd;
    struct sockaddr_in server_addr;
    char send_buffer[BUFFER_SIZE];
    char recv_buffer[BUFFER_SIZE];
    char username[32];
    int n;

    // 1. 创建套接字
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // 2. 准备服务器地址
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    // 3. 连接服务器
    printf("Connecting to server...\n");
    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("connect failed");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    // --- 第一步：登录逻辑 ---
    printf("Connected! Please enter your username to login: ");
    fgets(username, sizeof(username), stdin);
    // 去掉 fgets 读进来的换行符
    username[strcspn(username, "\r\n")] = 0;

    // 构造登录包：LOGIN:xxx
    memset(send_buffer, 0, BUFFER_SIZE);
    snprintf(send_buffer, BUFFER_SIZE, "LOGIN:%s\n", username);
    
    // 发送登录请求
    write(sock_fd, send_buffer, strlen(send_buffer));

    // 等待服务器的登录响应
    memset(recv_buffer, 0, BUFFER_SIZE);
    n = read(sock_fd, recv_buffer, BUFFER_SIZE - 1);
    if (n > 0) {
        printf("Server response: %s", recv_buffer);
        // 简单检查一下是否登录成功 (实际项目中应该解析具体的协议)
        if (strstr(recv_buffer, "OK") == NULL) {
            printf("Login failed, exiting.\n");
            close(sock_fd);
            return 0;
        }
    } else {
        printf("Server closed connection during login.\n");
        close(sock_fd);
        return 0;
    }

    printf("Login successful! You can now chat. Type 'exit' to quit.\n");
    printf("> ");

    // --- 第二步：正常聊天循环 ---
    while (1) {
        memset(send_buffer, 0, BUFFER_SIZE);
        memset(recv_buffer, 0, BUFFER_SIZE);

        // 读取用户输入
        if (fgets(send_buffer, BUFFER_SIZE, stdin) == NULL) break;

        // 检查退出
        if (strncmp(send_buffer, "exit", 4) == 0) {
            printf("Goodbye!\n");
            break;
        }

        // 发送消息给服务器
        write(sock_fd, send_buffer, strlen(send_buffer));

        // 等待服务器回显/回复
        n = read(sock_fd, recv_buffer, BUFFER_SIZE - 1);
        if (n <= 0) {
            printf("Server disconnected.\n");
            break;
        }

        printf("Server: %s> ", recv_buffer);
    }

    close(sock_fd);
    return 0;
}