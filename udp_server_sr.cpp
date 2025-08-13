// udp_server_sr.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stddef.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include "ev.h"

// ========== 协议/公共 ==========
enum pkt_type : uint8_t { PKT_DATA=1, PKT_ACK=2, PKT_FIN=3, PKT_FIN_ACK=4 };

#pragma pack(push, 1)
typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint16_t reserved;
    uint32_t seq;   // DATA: 分片序号；ACK: ack_base；FIN: total_chunks
    uint32_t len;   // DATA: payload 长度；其他：0
} hdr_t;

typedef struct {
    hdr_t    h;         // type=PKT_ACK, seq=ack_base
    uint64_t sack_bits; // bit i 表示 ack_base + i 是否已收
} ack_t;
#pragma pack(pop)

static inline int seq_lt(uint32_t a, uint32_t b){ return (int32_t)(a-b) < 0; }
static inline int seq_le(uint32_t a, uint32_t b){ return (int32_t)(a-b) <= 0; }

// ========== 参数 ==========
#define BUF_SIZE        2048
#define RECV_W          64
#define IDLE_TIMEOUT_S  120.0
#define SWEEP_PERIOD_S  30.0
#define ACK_DELAY_SEC   0.005  // 5ms 延迟 ACK 聚合

// ========== 会话 ==========
typedef struct {
    uint32_t len;
    uint8_t *buf;
} chunk_t;

typedef struct session_s {
    struct session_s *next;
    struct sockaddr_in peer;      // 会话键：src_ip + src_port
    FILE *fp;
    ev_tstamp last_active;
    char filename[128];

    // SR 状态
    uint32_t ack_base;
    uint64_t sack_bits;           // 相对 ack_base 的 64 位位图
    chunk_t  stash[RECV_W];       // 乱序缓存（索引 = seq - ack_base - 1）
    int      fin_seen;
    uint32_t final_seq_plus1;

    // 延迟 ACK
    struct ev_timer ack_timer;
    int ack_dirty;
} session_t;

typedef struct {
    int fd;
    struct ev_io io;
    struct ev_timer gc;
    session_t *head;
} server_ctx_t;

// ========== 工具 ==========
static const char *addr_to_str(const struct sockaddr_in *sa, char *buf, size_t len) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
    snprintf(buf, len, "%s:%u", ip, ntohs(sa->sin_port));
    return buf;
}

static void stash_free(chunk_t *c){ if (c->buf){ free(c->buf); c->buf=NULL; c->len=0; } }

static void send_ack_now(server_ctx_t *srv, session_t *s){
    ack_t a; memset(&a,0,sizeof(a));
    a.h.type = PKT_ACK;
    a.h.seq  = s->ack_base;
    a.sack_bits = s->sack_bits;
    sendto(srv->fd, &a, sizeof(a), 0, (struct sockaddr*)&s->peer, sizeof(s->peer));
    s->ack_dirty = 0;
}

static void flush_inorder(server_ctx_t *srv, session_t *s){
    while (s->sack_bits & 1ull){
        // bit0 对应 ack_base 的后一个（stash[0]）
        chunk_t *c = &s->stash[0];
        if (c->buf){
            fwrite(c->buf, 1, c->len, s->fp);
            stash_free(c);
        }
        // 缓存整体左移一位（O(64)）
        for (size_t i=0;i<RECV_W-1;i++) s->stash[i] = s->stash[i+1];
        s->stash[RECV_W-1].buf=NULL; s->stash[RECV_W-1].len=0;

        s->sack_bits >>= 1;
        s->ack_base++;
    }
}

// ========== 会话管理 ==========
static session_t* find_session(server_ctx_t *srv, const struct sockaddr_in *peer) {
    for (session_t *s = srv->head; s; s = s->next) {
        if (s->peer.sin_addr.s_addr == peer->sin_addr.s_addr &&
            s->peer.sin_port == peer->sin_port) {
            return s;
        }
    }
    return NULL;
}

static void ack_timer_cb(EV_P_ ev_timer *w, int revents){
    (void)revents;
    session_t *s = (session_t*)((char*)w - offsetof(session_t, ack_timer));
    // srv 指针：ack_timer 没有直接 backref；我们用 ev_userdata 存 srv* 更方便
    server_ctx_t *srv = (server_ctx_t *)ev_userdata(EV_A);
    if (s->ack_dirty) send_ack_now(srv, s);
}

static session_t* create_session(server_ctx_t *srv, const struct sockaddr_in *peer) {
    session_t *s = (session_t*)calloc(1, sizeof(session_t));
    if (!s) return NULL;
    s->peer = *peer;
    s->last_active = ev_time();
    s->ack_base = 0; s->sack_bits = 0;

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer->sin_addr, ip, sizeof(ip));
    snprintf(s->filename, sizeof(s->filename), "recv_%s_%u.bin", ip, ntohs(peer->sin_port));

    s->fp = fopen(s->filename, "wb");
    if (!s->fp){ perror("fopen"); free(s); return NULL; }

    // 链表头插
    s->next = srv->head; srv->head = s;

    // 延迟 ACK 定时器（会话内）
    ev_timer_init(&s->ack_timer, ack_timer_cb, ACK_DELAY_SEC, 0.0);
    // 注意：ack_timer_cb 里通过 ev_userdata 取 srv
    ev_timer_start(EV_DEFAULT, &s->ack_timer);

    char who[64];
    printf("[SRV] new session %s -> %s\n", addr_to_str(peer, who, sizeof(who)), s->filename);
    return s;
}

static void close_and_remove_session(server_ctx_t *srv, session_t *target) {
    if (target->fp){ fclose(target->fp); target->fp=NULL; }
    ev_timer_stop(EV_DEFAULT, &target->ack_timer);
    // 释放缓存
    for (size_t i=0;i<RECV_W;i++) stash_free(&target->stash[i]);

    session_t **pp = &srv->head;
    while (*pp) {
        if (*pp == target) { *pp = target->next; break; }
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

// ========== UDP 可读 ==========
static void udp_read_cb(EV_P_ struct ev_io *w, int revents) {
    (void)revents;
    server_ctx_t *srv = (server_ctx_t*)((char*)w - offsetof(server_ctx_t, io));

    uint8_t buf[BUF_SIZE];
    struct sockaddr_in peer; socklen_t plen = sizeof(peer);
    ssize_t n = recvfrom(srv->fd, buf, sizeof(buf), 0, (struct sockaddr*)&peer, &plen);
    if (n <= 0){
        if (errno==EAGAIN || errno==EWOULDBLOCK) return;
        perror("recvfrom"); return;
    }
    hdr_t *h = (hdr_t*)buf;

    session_t *s = find_session(srv, &peer);
    if (!s){ s = create_session(srv, &peer); if (!s) return; }
    s->last_active = ev_time();

    switch(h->type) {
        case PKT_DATA: 
            uint32_t seq = h->seq;
            uint32_t len = h->len;
            uint8_t *payload = buf + sizeof(hdr_t);

            if (seq_lt(seq, s->ack_base)){
                s->ack_dirty = 1;
                ev_timer_again(EV_A_ &s->ack_timer);
                return;
            }
            if (seq == s->ack_base){
                fwrite(payload, 1, len, s->fp);
                s->ack_base++;
                // 冲洗 stash 中连续已收
                flush_inorder(srv, s);
                s->ack_dirty = 1;
                ev_timer_again(EV_A_ &s->ack_timer);
                // 如果已经见过 FIN 且补齐，立即收尾
                if (s->fin_seen && s->ack_base == s->final_seq_plus1){
                    hdr_t fa = { .type=PKT_FIN_ACK };
                    sendto(srv->fd, &fa, sizeof(fa), 0, (struct sockaddr*)&s->peer, sizeof(s->peer));
                    close_and_remove_session(srv, s);
                }
                return;
            }
            // 在位图范围内
            if (seq - s->ack_base < RECV_W){
                size_t bit = seq - s->ack_base - 1;
                if (((s->sack_bits >> bit) & 1ull) == 0ull){
                    s->sack_bits |= (1ull << bit);
                    chunk_t *c = &s->stash[bit];
                    if (c->buf) free(c->buf);
                    c->buf = (uint8_t*)malloc(len);
                    memcpy(c->buf, payload, len);
                    c->len = len;
                }
                s->ack_dirty = 1;
                ev_timer_again(EV_A_ &s->ack_timer);
            } else {
                // 超出范围，仍提示 base（快速让对端收敛）
                s->ack_dirty = 1;
                ev_timer_again(EV_A_ &s->ack_timer);
            }
            break;

        case PKT_FIN: 
            s->fin_seen = 1;
            s->final_seq_plus1 = h->seq; // == 总分片数
            s->ack_dirty = 1;
            ev_timer_again(EV_A_ &s->ack_timer);
            if (s->ack_base == s->final_seq_plus1){
                hdr_t fa = { .type=PKT_FIN_ACK };
                sendto(srv->fd, &fa, sizeof(fa), 0, (struct sockaddr*)&s->peer, sizeof(s->peer));
                close_and_remove_session(srv, s);
            }
            break;
        
        default:
            fprintf(stderr, "Unknown packet type: %d\n", h->type);
            break;
    }
}

// ========== main ==========
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
    int flags = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, flags|O_NONBLOCK);

    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1){
        fprintf(stderr, "invalid bind ip: %s\n", bind_ip); return 1;
    }
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0){ perror("bind"); return 1; }

    server_ctx_t srv; memset(&srv, 0, sizeof(srv));
    srv.fd = fd;

    struct ev_loop *loop = EV_DEFAULT;
    ev_set_userdata(loop, &srv); // 让 ack_timer_cb 能拿到 srv*

    ev_io_init(&srv.io, udp_read_cb, fd, EV_READ);
    ev_io_start(loop, &srv.io);

    ev_timer_init(&srv.gc, sweep_idle_sessions, SWEEP_PERIOD_S, SWEEP_PERIOD_S);
    ev_timer_start(loop, &srv.gc);

    printf("[SRV] listening on %s:%d\n", bind_ip, port);
    ev_run(loop, 0);

    // 收尾
    session_t *s = srv.head, *next = NULL;
    while (s) { next = s->next; close_and_remove_session(&srv, s); s = next; }
    close(fd);
    return 0;
}
