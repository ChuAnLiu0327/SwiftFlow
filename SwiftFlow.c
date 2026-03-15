#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h> // 套接字API
#include <netinet/in.h> // 地址结构
#include <sys/epoll.h>  // epoll API
#include <fcntl.h>      // 文件控制API (设置非阻塞)
#include <errno.h>      // 错误码

#define PORT 8888          // 服务器监听端口
#define MAX_EVENTS 10      // epoll_wait 一次最多返回的事件数
#define BUFFER_SIZE 1024   // 缓冲区大小

// 将文件描述符设置为非阻塞模式
int set_nonblocking(int fd) {
    // 获取当前的文件描述符标志
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL failed");
        return -1;
    }
    // 添加非阻塞标志
    flags |= O_NONBLOCK;
    // 设置新的标志
    if (fcntl(fd, F_SETFL, flags) == -1) {
        perror("fcntl F_SETFL failed");
        return -1;
    }
    return 0;
}

int main() {
    int listen_fd, epoll_fd, conn_fd, nfds, n;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    struct epoll_event ev, events[MAX_EVENTS];
    char buffer[BUFFER_SIZE];

    // 1. 创建监听套接字 (socket)
    // AF_INET: IPv4, SOCK_STREAM: TCP, 0: 默认协议
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // 2. 设置套接字选项 (setsockopt)
    // SO_REUSEADDR: 允许端口复用，避免服务器重启时 "Address already in use" 错误
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    // 3. 将监听套接字设置为非阻塞
    if (set_nonblocking(listen_fd) == -1) {
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    // 4. 填充服务器地址结构体
    memset(&server_addr, 0, sizeof(server_addr)); // 清空结构体
    server_addr.sin_family = AF_INET;              // 地址族 IPv4
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // 监听所有网卡接口
    server_addr.sin_port = htons(PORT);            // 端口号 (主机序转网络序)

    // 5. 绑定地址和端口 (bind)
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    // 6. 开始监听 (listen)
    // 10: 等待连接队列的最大长度
    if (listen(listen_fd, 10) == -1) {
        perror("listen failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", PORT);

    // 7. 创建 epoll 实例 (epoll_create1)
    // 参数 size 在内核 2.6.8+ 后被忽略，传 >0 的数即可
    // epoll_create1(0) 等同于 epoll_create(1)
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1 failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    // 8. 将监听套接字添加到 epoll 监控中
    ev.events = EPOLLIN; // 监控可读事件 (有新连接进来也算可读)
    ev.data.fd = listen_fd; // 关联用户数据，这里存文件描述符
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        perror("epoll_ctl add listen_fd failed");
        close(epoll_fd);
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    // 9. 进入事件循环
    while (1) {
        // 等待事件发生 (epoll_wait)
        // -1: 无限阻塞直到有事件
        // nfds: 有多少个文件描述符就绪
        nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue; // 被信号中断，重试
            perror("epoll_wait failed");
            break; // 出错退出循环
        }

        // 遍历所有就绪的事件
        for (n = 0; n < nfds; n++) {
            // 情况 A: 监听套接字就绪，说明有新的客户端连接
            if (events[n].data.fd == listen_fd) {
                client_len = sizeof(client_addr);
                // 接受连接 (accept)
                conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
                if (conn_fd == -1) {
                    // 因为是非阻塞IO，这里可能会出现 EAGAIN/EWOULDBLOCK
                    // 表示虽然触发了事件，但连接已经被其他线程处理完了
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("accept failed");
                    }
                    continue;
                }

                printf("New connection accepted (fd: %d)\n", conn_fd);

                // 将新的连接套接字也设置为非阻塞
                if (set_nonblocking(conn_fd) == -1) {
                    close(conn_fd);
                    continue;
                }

                // 将新的连接套接字添加到 epoll 监控中
                // 使用边缘触发 (EPOLLET) 是更高效的做法，但这里为了简单先用水平触发 (默认)
                // 如果想试 ET，改为 ev.events = EPOLLIN | EPOLLET;
                ev.events = EPOLLIN; 
                ev.data.fd = conn_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev) == -1) {
                    perror("epoll_ctl add conn_fd failed");
                    close(conn_fd);
                }
            }
            // 情况 B: 客户端套接字就绪，处理数据
            else {
                int sock_fd = events[n].data.fd;
                // 清空缓冲区
                memset(buffer, 0, BUFFER_SIZE);
                
                // 读取数据
                // 注意：ET模式下必须循环读到返回 EAGAIN 为止
                int bytes_read = read(sock_fd, buffer, BUFFER_SIZE);
                
                if (bytes_read == -1) {
                    // 出错了
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("read failed");
                        close(sock_fd); // 关闭连接
                        // 从 epoll 中删除（其实 close 会自动移除，但显式调用是个好习惯）
                        // epoll_ctl(epoll_fd, EPOLL_CTL_DEL, sock_fd, NULL); 
                    }
                    continue;
                } 
                else if (bytes_read == 0) {
                    // 读取到 0 表示客户端关闭了连接
                    printf("Client disconnected (fd: %d)\n", sock_fd);
                    close(sock_fd);
                    continue;
                }
                else {
                    // 成功读取到数据
                    printf("Received from fd %d: %s", sock_fd, buffer);
                    
                    // 简单的回声逻辑：把数据发回去
                    // 注意：生产环境中 write 也可能写不完，需要关注 EPOLLOUT
                    write(sock_fd, buffer, bytes_read);
                }
            }
        }
    }

    // 10. 清理资源 (实际开发中需要确保退出逻辑能执行到这里)
    close(listen_fd);
    close(epoll_fd);
    return 0;
}