#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include "db_ops.h"
#include "uthash.h"
#include <cjson/cJSON.h>

#define PORT 50000      
#define MAX_EVENTS 10    
#define BUFFER_SIZE 1024  
#define ACCOUNT_SIZE 64
#define BACKLOG     1024

// 字体颜色
#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"

// 定义在线用户结构体
typedef struct online_user{
    char account[32];      // 键:账户名称,账号可以使用号码
    int sockfd;             // 值: 对应的socket文件描述符
    UT_hash_handle hh;      // 句柄: 让这个结构体可哈希
} online_user_t;
// 定义哈希表头指针，初始化为 NULL
online_user_t *online_users = NULL; // 全局哈希表

// 客户端连接的状态
typedef struct {
    int fd;               // 文件描述符
    int is_logged_in;     // 状态标记：0=未登录，1=已登录
    char username[32];    // 账户名
} client_ctx_t;

// 将文件描述符设置为非阻塞模式
int set_nonblocking(int fd) {
    // 获取当前的文件描述符标志
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror(COLOR_RED"fcntl F_GETFL failed");
        return -1;
    }
    // 添加非阻塞标志
    flags |= O_NONBLOCK;
    // 设置新的标志
    if (fcntl(fd, F_SETFL, flags) == -1) {
        perror(COLOR_RED"fcntl F_SETFL failed");
        return -1;
    }
    return 0;
}

// 插入或者更新用户
void add_user(const char* account,int sockfd){
    online_user_t *user;
    // 先检查这个账户是否存在
    HASH_FIND_STR(online_users,account,user);
    if(user){
        // 如果存在,更新新的连接
        printf(COLOR_YELLOW"INFO::Account exists, update socket connection\n");
        user->sockfd = sockfd;
    }else{
        // 用户并不存在,创建节点并插入
        printf(COLOR_YELLOW"INFO::The account [%s] does not exist. Create a node and insert the client information.\n",account);
        user = (online_user_t*)malloc(sizeof(online_user_t));
        strcpy(user->account,account);
        user->sockfd = sockfd;
        // 使用 HASH_ADD_STR 宏，参数：表头指针, 键字段名, 要添加的节点指针
        HASH_ADD_STR(online_users,account,user);
    }
}

// 根据账户查找用户，返回找到的结构体指针，没找到则返回 NULL
online_user_t* find_user(const char* account){
    online_user_t* user = NULL;
    HASH_FIND_STR(online_users,account,user);
    return user;
}

// 查找对应账户客户端的文件描述符
int find_user_sockfd(const char* account) {
    online_user_t* user = NULL;
    HASH_FIND_STR(online_users, account, user);
    int sockfd = user ? user->sockfd : -1;
    return sockfd;
}

// 根据账户删除用户
int delet_user(const char* account) {
    online_user_t* user = NULL;
    HASH_FIND_STR(online_users, account, user);
    if (user) {
        HASH_DEL(online_users, user);
        free(user);
        return 1;
    }
    printf(COLOR_YELLOW"INFO::The deleted user account does not exist.\n");
    return -1;
}

// 通过文件描述符删除用户
void remove_user_by_sockfd(int sockfd) {
    online_user_t *user, *tmp;
    HASH_ITER(hh, online_users, user, tmp) {
        if (user->sockfd == sockfd) {
            HASH_DEL(online_users, user);
            printf(COLOR_GREEN"INFO::User %s has been removed from the online list.\n", user->account);
            free(user);
            break;
        }
    }
}

// 群发消息给所有在线用户（排除发送者自己）
void broadcast_message(const char* sender, const char* message) {
    online_user_t *user, *tmp;
    char send_buffer[1024];
    snprintf(send_buffer, sizeof(send_buffer), "[Broadcast from %s] %s", sender, message);
    
    // 遍历哈希表所有在线用户
    HASH_ITER(hh, online_users, user, tmp) {
        // 排除发送者自己
        if (strcmp(user->account, sender) != 0) {
            write(user->sockfd, send_buffer, strlen(send_buffer));
            printf(COLOR_GREEN"BROADCAST FROM:[%s] TO:[%s] MSG:[%s]\n", sender, user->account, message);
        }
    }
}

int main() 
{
    // 初始化聊天记录的数据库
    sqlite3* chatMessageDB = sqliteInit_chatMessageDB();
    // 定义变量
    int listen_fd, epoll_fd, conn_fd, nfds, n;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    struct epoll_event ev, events[MAX_EVENTS];
    char buffer[BUFFER_SIZE];
    // 1. 创建监听套接字 (socket)
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror(COLOR_RED"INFO::Socket creation failed");
        exit(EXIT_FAILURE);
    }
    // 2. 设置套接字选项 (setsockopt)
    // SO_REUSEADDR: 允许端口复用，避免"Address already in use" 错误
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror(COLOR_RED"INFO::setsockopt failed");
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
        perror(COLOR_RED"INFO::Socket binding to port failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    // 6. 开始监听 (listen)
    if (listen(listen_fd, 10) == -1) {
        perror(COLOR_RED"INFO::Server listening failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    printf(COLOR_GREEN"INFO::Server listening on port %d...\n", PORT);
    // 7. 创建 epoll 实例 (epoll_create1)
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror(COLOR_RED"INFO::epoll_create1 failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    // 8. 将监听套接字添加到 epoll 监控中
    ev.events = EPOLLIN; // 监控可读事件 (有新连接进来也算可读)
    ev.data.fd = listen_fd; // 关联用户数据，这里存文件描述符
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        perror(COLOR_RED"INFO::epoll_ctl add listen_fd failed");
        close(epoll_fd);
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    while (1) 
    {
        nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (n = 0; n < nfds; n++) 
        {
            if (events[n].data.fd == listen_fd) 
            {
                // --- 处理新连接 ---
                conn_fd = accept(listen_fd, NULL, NULL);
                if (conn_fd == -1) continue;
                
                printf(COLOR_GREEN"INFO::New client connected (fd: %d). Waiting for login...\n", conn_fd);
                set_nonblocking(conn_fd); // 设置为非阻塞

                // 为这个客户端分配一个上下文结构体
                client_ctx_t *new_client = (client_ctx_t *)malloc(sizeof(client_ctx_t));
                new_client->fd = conn_fd;
                new_client->is_logged_in = 0; // 初始状态：未登录
                memset(new_client->username, 0, sizeof(new_client->username));

                // 将上下文指针绑定到 epoll 的 data 字段
                ev.events = EPOLLIN;
                ev.data.ptr = new_client; 
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev);

            } else 
            {
                // --- 处理客户端数据 ---
                client_ctx_t *client = (client_ctx_t *)events[n].data.ptr;
                int sock_fd = client->fd;
                memset(buffer, 0, BUFFER_SIZE);
                int bytes_read = read(sock_fd, buffer, BUFFER_SIZE - 1);
                if (bytes_read <= 0) {
                    // 连接断开或出错
                    printf(COLOR_RED"INFO::Client fd %d disconnected.\n", sock_fd);
                    remove_user_by_sockfd(sock_fd); // 从哈希表移除
                    close(sock_fd);
                    free(client); // 释放上下文内存
                    continue;
                }
                // --- 核心业务逻辑分支 ---
                if (client->is_logged_in == 0) 
                {
                    // [状态 1] 未登录：认为这条消息是登录请求
                    // 简单协议解析：假设消息格式是 "LOGIN:xxx"
                    if (strncmp(buffer, "LOGIN:", 6) == 0) {
                        char *username = buffer + 6;
                        // 去掉末尾的换行符
                        username[strcspn(username, "\r\n")] = 0;
                        // 保存状态
                        strncpy(client->username, username, sizeof(client->username)-1);
                        client->is_logged_in = 1;
                        // 加入哈希表
                        add_user(client->username, sock_fd);
                        const char *ok_msg = "Login OK!\n";
                        write(sock_fd, ok_msg, strlen(ok_msg));
                    } else {
                        const char *err_msg = "Please login first: LOGIN:yourname\n";
                        write(sock_fd, err_msg, strlen(err_msg));
                    }
                } else {
                    // [状态 2] 已登录：正常业务处理
                    printf(COLOR_GREEN"[Msg from %s] %s", client->username, buffer);
                    cJSON *root = cJSON_Parse(buffer);
                    if (root == NULL) { // 解析失败
                        const char *err = "Error: Invalid JSON format!\n";
                        write(sock_fd, err, strlen(err));
                        continue;
                    }

                    // 解析并校验all字段
                    cJSON *all = cJSON_GetObjectItem(root,"all");
                    int is_broadcast = 0; // 默认私发
                    if(all !=NULL){
                        if(!cJSON_IsBool(all)){
                            const char *err = "Error: 'all' field must be boolean!\n";
                            write(sock_fd, err, strlen(err));
                            cJSON_Delete(root);
                            continue;
                        }
                        is_broadcast = cJSON_IsTrue(all);
                    }

                    // 解析并校验 msg 字段
                    cJSON *msg = cJSON_GetObjectItem(root, "msg");
                    if (msg == NULL || !cJSON_IsString(msg)) {
                        const char *err = "Error: JSON missing 'msg' field or field is not string!\n";
                        write(sock_fd, err, strlen(err));
                        cJSON_Delete(root);
                        continue;
                    }
                    const char *message = msg->valuestring;

                    if(is_broadcast)
                    {
                        // 群发的逻辑
                        broadcast_message(client->username,message);
                        // 保存群发记录到数据库
                        insert_messagee(chatMessageDB,client->username,"all",message);
                        // 给发送者反馈
                        char success[1024];
                        snprintf(success,sizeof(success),"Broadcast success! Message: %s\n",message);
                        write(sock_fd,success,strlen(success));
                    }else{
                        // 还是私发的逻辑
                        cJSON *to = cJSON_GetObjectItem(root,"to");
                        if(to == NULL || !cJSON_IsString(to)){
                            const char *err = "Error: JSON missing 'to' field or field is not string!\n";
                            write(sock_fd, err, strlen(err));
                            cJSON_Delete(root);
                            continue;
                        }
                        const char *target_account = to->valuestring;
                        // 查找文件描述符
                        // 通过target_account查找目标的文件描述符
                        int target_sock = find_user_sockfd(target_account);
                        if(target_sock != -1){
                            char message_buffer[1024];
                            snprintf(message_buffer,sizeof(message_buffer),"%s",message);
                            write(target_sock,message_buffer,strlen(message_buffer));
                            printf(COLOR_GREEN"FROM:[%s] TO:[%s] MSG:[%s]\n",client->username,target_account,message);
                        }else{
                            // 发送失败或者用户不在线
                            printf(COLOR_RED"FROM:[%s] TO:[%s] MSG:[%s] ,The target users are not online\n",client->username,target_account,message);
                            char erro[1024];
                            snprintf(erro,sizeof(erro),"The target user '%s' does not exist or is not online.\n",target_account);
                            write(client->fd,erro,strlen(erro));
                        }
                        // 存到数据库中
                        insert_messagee(chatMessageDB,client->username,target_account,message);
                    }
                    cJSON_Delete(root);  
                }  
            }
        }
    }
    return 0;
}