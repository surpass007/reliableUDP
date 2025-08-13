#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <ev.h> // libev 头文件

#define PORT 7001
#define BACKLOG 128
#define BUF_SIZE 1024

// 客户端会话结构
typedef struct {
    struct ev_io read_watcher;
    int fd;
    void *data; // 可选数据指针
} client_t;

// 设置非阻塞
static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 客户端读回调
static void client_read_cb(EV_P_ struct ev_io *w, int revents) {
    client_t *client = (client_t *)w;
    char buf[BUF_SIZE];
    ssize_t n = recv(client->fd, buf, sizeof(buf), 0);
    if (n > 0) {
        printf("Received %zd bytes from client %d\n", n, client->fd);
        buf[n] = '\0'; // 确保字符串结束
        printf("Data: %s\n", buf);
        send(client->fd, buf, n, 0); // echo 回去
    } else {
        if (n < 0) perror("recv");
        ev_io_stop(EV_A_ w);
        close(client->fd);
        free(client);
    }
}

// 新连接回调
static void accept_cb(EV_P_ struct ev_io *w, int revents) {
    int server_fd = w->fd;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("accept");
        return;
    }
    set_nonblock(client_fd);

    client_t *client = (client_t*)malloc(sizeof(client_t)); 
    client->fd = client_fd;
    ev_io_init(&client->read_watcher, client_read_cb, client_fd, EV_READ);
    ev_io_start(EV_A_ &client->read_watcher);

    printf("New connection: %s:%d\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
}

int main(int argc, char *argv[]) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblock(server_fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    uint32_t port = PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port number: %s\n", argv[1]);
            return 1;
        }
        printf("Using port %s\n", argv[1]);
    } else {
        printf("Using default port %d\n", PORT);
    }

    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        return 1;
    }

    printf("Listening on port %d...\n", port);

    struct ev_loop *loop = EV_DEFAULT;
    struct ev_io accept_watcher;
    ev_io_init(&accept_watcher, accept_cb, server_fd, EV_READ);
    ev_io_start(loop, &accept_watcher);

    ev_run(loop, 0);
    close(server_fd);
    return 0;
}
