// udp_client_sr.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

// ========== 发送端参数 ==========
#define WIN         64
#define MSS         1200
#define RTO_SEC     0.20    // 200ms 重传超时
#define TICK_SEC    0.01    // 10ms 发送/重传调度周期
#define MAX_PKT     1500

// ========== 发送端上下文 ==========
typedef struct {
    int fd;
    struct sockaddr_in server;

    FILE *fp;
    uint32_t base;          // 已确认的下一个序号
    uint32_t next_seq;      // 下一个可发送序号
    uint32_t total_chunks;  // 总分片（用于 FIN.seq）
    int      eof_sent;      // 已读完文件（窗口清空后发 FIN）
    int      fin_sent;
    int      fin_acked;
    double   fin_last_send;

    struct {
        size_t   len;
        uint8_t *buf;       // 完整 DATA 包（含 hdr）
        double   last_send;
        int      acked;
    } win[WIN];

    // libev
    struct ev_timer timer;  // 周期调度 timer（发新包/重传/发FIN）
    struct ev_io    io;     // 收 ACK/FIN-ACK
} client_ctx_t;

// ========== 工具 ==========
static size_t make_data(uint8_t *out, uint32_t seq, const uint8_t *payload, uint32_t len){
    hdr_t *h = (hdr_t*)out;
    h->type=PKT_DATA; h->flags=0; h->reserved=0;
    h->seq =seq;      h->len   =len;
    memcpy(out+sizeof(hdr_t), payload, len);
    return sizeof(hdr_t)+len;
}

static void send_data(client_ctx_t *ctx, uint32_t idx){
    sendto(ctx->fd, ctx->win[idx].buf, ctx->win[idx].len, 0,
           (struct sockaddr*)&ctx->server, sizeof(ctx->server));
    ctx->win[idx].last_send = ev_time();
}

// ========== 读 ACK/FIN-ACK ==========
static void on_read(EV_P_ struct ev_io *w, int revents){
    (void)revents;
    client_ctx_t *ctx = (client_ctx_t *)((char*)w - offsetof(client_ctx_t, io));

    uint8_t buf[MAX_PKT];
    ssize_t n = recv(ctx->fd, buf, sizeof(buf), 0);
    if (n <= 0) return;

    hdr_t *h = (hdr_t*)buf;
    if (h->type == PKT_ACK && n >= (ssize_t)sizeof(ack_t)){
        ack_t *a = (ack_t*)buf;
        uint32_t ackb = a->h.seq;
        uint64_t sack = a->sack_bits;

        // 弹出 [base, ackb)
        while (seq_lt(ctx->base, ackb)){
            uint32_t idx = ctx->base % WIN;
            if (ctx->win[idx].buf){ free(ctx->win[idx].buf); ctx->win[idx].buf=NULL; }
            ctx->win[idx].acked=1;
            ctx->base++;
        }
        // 标记 sack 中的已收
        for (uint32_t i=0; i<WIN && seq_lt(ackb+i, ctx->next_seq) && i<64; ++i){
            if (sack & (1ull<<i)){
                uint32_t s = ackb+i;
                uint32_t idx = s % WIN;
                if (ctx->win[idx].buf){ free(ctx->win[idx].buf); ctx->win[idx].buf=NULL; }
                ctx->win[idx].acked=1;
            }
        }
        // 再次滑动连续 ACK
        while (seq_lt(ctx->base, ctx->next_seq)){
            uint32_t idx = ctx->base % WIN;
            if (ctx->win[idx].acked) ctx->base++; else break;
        }
    } else if (h->type == PKT_FIN_ACK){
        ctx->fin_acked = 1;
        printf("[CLI] FIN-ACK received. Transfer done.\n");
        ev_break(EV_A_ EVBREAK_ALL);
    }
}

// ========== 周期调度：填充窗口/重传/FIN ==========
static void send_batch(EV_P_ struct ev_timer *w, int revents){
    (void)revents;
    client_ctx_t *ctx = (client_ctx_t *)((char*)w - offsetof(client_ctx_t, timer));

    // 1) 尽量填充窗口并发送新包
    while (!ctx->eof_sent){
        uint32_t inwin = ctx->next_seq - ctx->base;
        if (inwin >= WIN) break;

        uint8_t payload[MSS];
        size_t n = fread(payload, 1, sizeof(payload), ctx->fp);
        if (n == 0){ ctx->eof_sent = 1; break; }

        uint8_t *pkt = (uint8_t*)malloc(sizeof(hdr_t)+n);
        size_t m = make_data(pkt, ctx->next_seq, payload, (uint32_t)n);
        uint32_t idx = ctx->next_seq % WIN;
        if (ctx->win[idx].buf) free(ctx->win[idx].buf);
        ctx->win[idx].buf = pkt; ctx->win[idx].len = m;
        ctx->win[idx].acked = 0; ctx->win[idx].last_send = 0.0;
        send_data(ctx, idx);
        ctx->next_seq++;
        ctx->total_chunks = ctx->next_seq;
    }

    // 2) 超时重传
    double now = ev_time();
    for (uint32_t s = ctx->base; seq_lt(s, ctx->next_seq); ++s){
        uint32_t idx = s % WIN;
        if (ctx->win[idx].buf && !ctx->win[idx].acked){
            if (now - ctx->win[idx].last_send > RTO_SEC){
                send_data(ctx, idx);
            }
        }
    }

    // 3) 所有数据确认后，发送 FIN
    if (!ctx->fin_sent && ctx->eof_sent){
        int all_acked = 1;
        for (uint32_t s=ctx->base; seq_lt(s, ctx->next_seq); ++s){
            uint32_t idx = s % WIN;
            if (ctx->win[idx].buf && !ctx->win[idx].acked){ all_acked=0; break; }
        }
        if (all_acked){
            hdr_t fin = { .type=PKT_FIN, .flags=0, .reserved=0,
                          .seq=ctx->total_chunks, .len=0 };
            sendto(ctx->fd, &fin, sizeof(fin), 0,
                   (struct sockaddr*)&ctx->server, sizeof(ctx->server));
            ctx->fin_sent = 1;
            ctx->fin_last_send = now;
            // 继续走定时器，等待 FIN-ACK 或重发 FIN
        }
    }

    // 4) FIN 超时重发
    if (ctx->fin_sent && !ctx->fin_acked && (now - ctx->fin_last_send > RTO_SEC)){
        hdr_t fin = { .type=PKT_FIN, .flags=0, .reserved=0,
                      .seq=ctx->total_chunks, .len=0 };
        sendto(ctx->fd, &fin, sizeof(fin), 0,
               (struct sockaddr*)&ctx->server, sizeof(ctx->server));
        ctx->fin_last_send = now;
    }

    // 维持周期
    ev_timer_again(EV_A_ w); // 维持 TICK 周期
}

int main(int argc, char **argv){
    if (argc < 4){
        fprintf(stderr, "Usage: %s <server_ip> <port> <file>\n", argv[0]);
        return 1;
    }
    const char* sip = argv[1];
    int port = atoi(argv[2]);
    const char* file = argv[3];

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0){ perror("socket"); return 1; }
    int flags = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, flags|O_NONBLOCK);

    client_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.fd = fd;
    ctx.server.sin_family = AF_INET;
    ctx.server.sin_port   = htons(port);
    if (inet_pton(AF_INET, sip, &ctx.server.sin_addr) != 1){
        fprintf(stderr, "invalid ip: %s\n", sip); return 1;
    }

    ctx.fp = fopen(file, "rb");
    if (!ctx.fp){ perror("fopen"); return 1; }

    struct ev_loop *loop = EV_DEFAULT;

    ev_io_init(&ctx.io, on_read, ctx.fd, EV_READ);
    ev_io_start(loop, &ctx.io);

    ev_timer_init(&ctx.timer, send_batch, 0.0, TICK_SEC);
    ev_timer_start(loop, &ctx.timer);

    printf("[CLI] start sending %s to %s:%d\n", file, sip, port);
    ev_run(loop, 0);

    // 清理
    for (size_t i=0;i<WIN;i++) if (ctx.win[i].buf) free(ctx.win[i].buf);
    if (ctx.fp) fclose(ctx.fp);
    close(ctx.fd);
    return 0;
}
