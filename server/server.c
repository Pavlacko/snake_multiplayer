#include "../common/net.h"
#include "../common/protocol.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 5555

typedef struct {
    int fd;
    int player_id;
} ClientCtx;

static volatile sig_atomic_t g_running = 1;
static pthread_mutex_t g_id_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_next_id = 1;

static int alloc_player_id(void) {
    pthread_mutex_lock(&g_id_mtx);
    int id = g_next_id++;
    pthread_mutex_unlock(&g_id_mtx);
    return id;
}

static void *client_thread(void *arg) {
    ClientCtx *ctx = (ClientCtx *)arg;

    uint16_t type = 0;
    uint32_t len = 0;

    if (net_recv_header(ctx->fd, &type, &len) != 0) goto done;
    if (type != MSG_HELLO || len != sizeof(MsgHello)) goto done;

    MsgHello hello;
    if (net_recv_all(ctx->fd, &hello, sizeof(hello)) != 0) goto done;

    char name[SNAKE_NAME_MAX + 1];
    memcpy(name, hello.name, SNAKE_NAME_MAX);
    name[SNAKE_NAME_MAX] = '\0';

    ctx->player_id = alloc_player_id();

    MsgWelcome w;
    w.player_id = ctx->player_id;
    if (net_send_msg(ctx->fd, MSG_WELCOME, &w, sizeof(w)) != 0) goto done;

    fprintf(stderr, "[server] player %d connected (name='%s')\n", ctx->player_id, name);

    while (g_running) {
        if (net_recv_header(ctx->fd, &type, &len) != 0) break;

        while (len > 0) {
            char tmp[256];
            uint32_t chunk = len > sizeof(tmp) ? (uint32_t)sizeof(tmp) : len;
            if (net_recv_all(ctx->fd, tmp, chunk) != 0) goto done;
            len -= chunk;
        }

        if (type == MSG_PING) {
            (void)net_send_msg(ctx->fd, MSG_PONG, NULL, 0);
        } else if (type == MSG_BYE) {
            break;
        }
    }

done:
    fprintf(stderr, "[server] player %d disconnected\n", ctx->player_id);
    close(ctx->fd);
    free(ctx);
    return NULL;
}

static void on_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

int main(void) {
    signal(SIGINT, on_sigint);

    int listen_fd = net_listen_tcp(PORT);
    if (listen_fd < 0) {
        perror("net_listen_tcp");
        return 1;
    }

    fprintf(stderr, "[server] listening on 0.0.0.0:%d\n", PORT);

    while (g_running) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (!g_running) break;
            perror("accept");
            continue;
        }

        ClientCtx *ctx = (ClientCtx *)calloc(1, sizeof(ClientCtx));
        if (!ctx) { close(client_fd); continue; }
        ctx->fd = client_fd;

        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, ctx) != 0) {
            close(client_fd);
            free(ctx);
            continue;
        }
        pthread_detach(th);
    }

    close(listen_fd);
    fprintf(stderr, "[server] shutdown\n");
    return 0;
}
