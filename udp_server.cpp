// udp_server_file_multi.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stddef.h>

#include "ev.h"


#define BUF_SIZE        2048
#define IDLE_TIMEOUT_S  120.0   // 会话空闲超时（秒）
#define SWEEP_PERIOD_S  30.0    // 定期清理周期（秒）

typedef struct session_s {
    struct session_s *next;
    struct sockaddr_in peer;   // 会话键：src_ip + src_port
    FILE *fp;
    ev_tstamp last_active;
    char filename[128];
} session_t;

typedef struct {
    int fd;
    struct ev_io io;
    struct ev_timer gc;        // 定期清理 timer
    session_t *head;           // 简单链表保存会话
} server_ctx_t;

static const char *addr_to_str(const struct sockaddr_in *sa, char *buf, size_t len) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
    snprintf(buf, len, "%s:%u", ip, ntohs(sa->sin_port));
    return buf;
}

static session_t* find_session(server_ctx_t *srv, const struct sockaddr_in *peer) {
    for (session_t *s = srv->head; s; s = s->next) {
        if (s->peer.sin_addr.s_addr == peer->sin_addr.s_addr &&
            s->peer.sin_port == peer->sin_port) {
            return s;
        }
    }
    return NULL;
}

static session_t* create_session(server_ctx_t *srv, const struct sockaddr_in *peer) {
    session_t *s = (session_t*)calloc(1, sizeof(session_t));
    if (!s) return NULL;
    s->peer = *peer;
    s->last_active = ev_time();

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer->sin_addr, ip, sizeof(ip));
    snprintf(s->filename, sizeof(s->filename), "recv_%s_%u.bin", ip, ntohs(peer->sin_port));

    s->fp = fopen(s->filename, "wb");
    if (!s->fp) {
        perror("fopen");
        free(s);
        return NULL;
    }

    // 插到链表头
    s->next = srv->head;
    srv->head = s;

    char who[64];
    printf("[SRV] new session %s -> %s\n", addr_to_str(peer, who, sizeof(who)), s->filename);
    return s;
}

static void close_and_remove_session(server_ctx_t *srv, session_t *target) {
    // 关闭文件，从链表里摘除
    if (target->fp) {
        fclose(target->fp);
        target->fp = NULL;
    }
    // remove from list
    session_t **pp = &srv->head;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            break;
        }
        pp = &((*pp)->next);
    }
    char who[64];
    printf("[SRV] end session %s (file saved: %s)\n",
           addr_to_str(&target->peer, who, sizeof(who)), target->filename);
    free(target);
}

static void sweep_idle_sessions(EV_P_ struct ev_timer *w, int revents) {
    (void)revents;
    server_ctx_t *srv = (server_ctx_t*)((char*)w - offsetof(server_ctx_t, gc));
    ev_tstamp now = ev_time();

    session_t *s = srv->head, *next = NULL;
    while (s) {
        next = s->next;
        if (now - s->last_active > IDLE_TIMEOUT_S) {
            close_and_remove_session(srv, s);
        }
        s = next;
    }
}

// UDP 可读回调：按来源（src_ip:src_port）路由到对应会话
static void udp_read_cb(EV_P_ struct ev_io *w, int revents) {
    (void)revents;
    server_ctx_t *srv = (server_ctx_t*)((char*)w - offsetof(server_ctx_t, io));
    int fd = srv->fd;

    char buf[BUF_SIZE + 1];
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);

    ssize_t n = recvfrom(fd, buf, BUF_SIZE, 0, (struct sockaddr*)&peer, &plen);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        perror("recvfrom");
        return;
    }
    buf[n] = '\0';

    session_t *sess = find_session(srv, &peer);
    if (!sess) {
        sess = create_session(srv, &peer);
        if (!sess) return;
    }
    sess->last_active = ev_time();

    if (n == 3 && memcmp(buf, "EOF", 3) == 0) {
        // 完成该会话
        close_and_remove_session(srv, sess);
        return;
    }

    // 写入该会话对应的文件
    size_t wlen = fwrite(buf, 1, n, sess->fp);
    if (wlen != (size_t)n) {
        perror("fwrite");
        // 写失败也结束会话，避免继续写坏数据
        close_and_remove_session(srv, sess);
        return;
    }
}

int main(int argc, char **argv) {
    const char *bind_ip = argc > 1 ? argv[1] : "0.0.0.0";
    int port = argc > 2 ? atoi(argv[2]) : 7000;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif

    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid bind ip: %s\n", bind_ip);
        return 1;
    }
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    server_ctx_t srv; memset(&srv, 0, sizeof(srv));
    srv.fd = fd;

    printf("[SRV] listening on %s:%d\n", bind_ip, port);

    struct ev_loop *loop = EV_DEFAULT;
    ev_io_init(&srv.io, udp_read_cb, fd, EV_READ);
    ev_io_start(loop, &srv.io);

    // 定期清理闲置会话
    ev_timer_init(&srv.gc, sweep_idle_sessions, SWEEP_PERIOD_S, SWEEP_PERIOD_S);
    ev_timer_start(loop, &srv.gc);

    ev_run(loop, 0);

    // 收尾：关掉所有会话
    session_t *s = srv.head, *next = NULL;
    while (s) { next = s->next; close_and_remove_session(&srv, s); s = next; }

    close(fd);
    return 0;
}
