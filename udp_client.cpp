#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stddef.h>

#include "ev.h"

#define BUF_SIZE 1024
#define PACKETS_PER_CYCLE 100

// ===== 全局变量 =====
int g_interval_ms = 500;           // 每个周期间隔（毫秒）
const char *g_file_path = "send.txt"; // 要发送的文件名

typedef struct {
    int fd;
    struct sockaddr_in server;
    struct ev_timer timer;
    FILE *fp;
} client_ctx_t;

static void send_batch(EV_P_ struct ev_timer *w, int revents) {
    client_ctx_t *ctx = (client_ctx_t *)((char*)w - offsetof(client_ctx_t, timer));
    char buf[BUF_SIZE];
    int packets_sent = 0;

    size_t n = fread(buf, 1, sizeof(buf), ctx->fp);
    if (n > 0) {
        sendto(ctx->fd, buf, n, 0,
                (struct sockaddr*)&ctx->server, sizeof(ctx->server));
    }
    if (n < sizeof(buf)) { // EOF
        sendto(ctx->fd, "EOF", 3, 0,
                (struct sockaddr*)&ctx->server, sizeof(ctx->server));
        printf("[CLI] File transfer complete\n");
        ev_timer_stop(EV_A_ w);
        fclose(ctx->fp);
        close(ctx->fd);
        return;
    }

    printf("[CLI] Sent %d packets, next batch in %d ms\n", packets_sent, g_interval_ms);
    ev_timer_set(&ctx->timer, g_interval_ms / 1000.0, 0.0);
    ev_timer_start(EV_A_ &ctx->timer);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <server_ip> <port> <file> [interval_ms]\n", argv[0]);
        return 1;
    }

    const char* sip = argv[1];
    int port = atoi(argv[2]);
    g_file_path = argv[3];
    if (argc >= 5) g_interval_ms = atoi(argv[4]);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    client_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fd = fd;
    ctx.server.sin_family = AF_INET;
    ctx.server.sin_port = htons(port);
    inet_pton(AF_INET, sip, &ctx.server.sin_addr);

    ctx.fp = fopen(g_file_path, "rb");
    if (!ctx.fp) { perror("fopen"); return 1; }

    struct ev_loop *loop = EV_DEFAULT;
    ev_timer_init(&ctx.timer, send_batch, 0.0, 0.0); // 启动时立即发送第一批
    ev_timer_start(loop, &ctx.timer);

    ev_run(loop, 0);
    return 0;
}
