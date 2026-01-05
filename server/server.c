#define _POSIX_C_SOURCE 200809L

#include "../common/net.h"
#include "../common/protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT 5555
#define SERVER_INFO_FILE "server.info"

typedef struct {
    int fd;
    int player_id;
} ClientCtx;

static volatile sig_atomic_t g_running = 1;
static pthread_mutex_t g_id_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_next_id = 1;

static void on_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static void server_detach(void) {
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    if (setsid() < 0) {
        // aj tak môžeme pokračovať; je to len “nice to have”
    }
}

static int alloc_player_id(void) {
    pthread_mutex_lock(&g_id_mtx);
    int id = g_next_id++;
    pthread_mutex_unlock(&g_id_mtx);
    return id;
}

static void write_server_info(int port) {
    FILE *f = fopen(SERVER_INFO_FILE, "w");
    if (!f) return;
    fprintf(f, "HOST=127.0.0.1\nPORT=%d\n", port);
    fclose(f);
}

static void *client_thread(void *arg) {
    ClientCtx *ctx = (ClientCtx *)arg;
    int fd = ctx->fd;

    // ---- handshake: HELLO -> WELCOME ----
    uint16_t type = 0;
    uint32_t len = 0;

    if (net_recv_header(fd, &type, &len) != 0) goto done;
    if (type != MSG_HELLO || len != sizeof(MsgHello)) goto done;

    MsgHello hello;
    if (net_recv_all(fd, &hello, (int)sizeof(hello)) != 0) goto done;

    ctx->player_id = alloc_player_id();

    MsgWelcome w;
    w.player_id = htonl((int32_t)ctx->player_id);
    if (net_send_msg(fd, MSG_WELCOME, &w, (uint32_t)sizeof(w)) != 0) goto done;

    // ---- loop ----
    while (g_running) {
        if (net_recv_header(fd, &type, &len) != 0) break;

        if (type == MSG_PING) {
            // ping má len 0, ignor
            if (len > 0) {
                // ak by prišlo niečo navyše, odsaj
                char *tmp = (char *)malloc(len);
                if (!tmp) break;
                int r = net_recv_all(fd, tmp, (int)len);
                free(tmp);
                if (r != 0) break;
            }
        } else if (type == MSG_BYE) {
            // BYE len 0
            if (len > 0) {
                char *tmp = (char *)malloc(len);
                if (!tmp) break;
                int r = net_recv_all(fd, tmp, (int)len);
                free(tmp);
                if (r != 0) break;
            }
            break;
        } else {
            // neznámy typ -> odsaj payload a pokračuj / alebo break (tu break)
            if (len > 0) {
                char *tmp = (char *)malloc(len);
                if (!tmp) break;
                int r = net_recv_all(fd, tmp, (int)len);
                free(tmp);
                if (r != 0) break;
            }
            break;
        }
    }

done:
    close(fd);
    free(ctx);
    return NULL;
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) port = DEFAULT_PORT;
    }

    server_detach();
    signal(SIGINT, on_sigint);

    int listen_fd = net_listen_tcp(port);
    if (listen_fd < 0) {
        perror("net_listen_tcp");
        return 1;
    }

    // P10: definícia IPC rozhrania pri štarte servera
    write_server_info(port);

    fprintf(stderr, "[server] listening on 0.0.0.0:%d\n", port);

    while (g_running) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (!g_running) break;
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        ClientCtx *ctx = (ClientCtx *)calloc(1, sizeof(ClientCtx));
        if (!ctx) {
            close(client_fd);
            continue;
        }
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
