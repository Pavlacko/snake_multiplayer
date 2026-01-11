#define _POSIX_C_SOURCE 200809L

#include "../common/net.h"
#include "../common/protocol.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 5555

static volatile sig_atomic_t g_running = 1;

static void on_sigint(int sig) { (void)sig; g_running = 0; }

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint16_t u16min(uint16_t a, uint16_t b) { return (a < b) ? a : b; }

typedef struct {
    bool used;
    bool connected;
    bool active;
    bool alive;
    bool paused;
    bool ready;
    int fd;
    char name[SNAKE_NAME_MAX];
    uint8_t dir;
    uint8_t pending_dir;
    uint16_t score;
    uint16_t len;
    Cell body[1024];
    uint64_t spawn_ms;        
    uint32_t time_ms_final; 
} Player;

typedef struct {
    Cell pos;
    uint32_t visited_mask;
} Fruit;

typedef struct {
    int w, h;
    uint8_t mode;
    uint8_t world;
    uint16_t time_limit_sec;
    char map_path[256];
    uint8_t *map;

    uint32_t tick_ms;
    uint64_t start_ms;
    uint64_t last_tick_ms;

    uint16_t global_freeze_ms;
    uint64_t last_no_players_ms;

    Player players[MAX_PLAYERS];
    Fruit fruits[MAX_FRUITS];
    uint8_t num_fruits;

    bool game_over;
    pthread_mutex_t mtx;
} Game;

static int idx(Game *g, int x, int y) { return y * g->w + x; }

static bool in_bounds(Game *g, int x, int y) {
    return x >= 0 && x < g->w && y >= 0 && y < g->h;
}

static bool is_obstacle(Game *g, int x, int y) {
    if (g->world == 0) return false;
    if (!in_bounds(g, x, y)) return true;
    return g->map[idx(g, x, y)] == 1;
}

static void dir_delta(uint8_t dir, int *dx, int *dy) {
    *dx = 0; *dy = 0;
    switch (dir) {
        case 0: *dy = -1; break;
        case 1: *dx =  1; break;
        case 2: *dy =  1; break;
        case 3: *dx = -1; break;
        default: break;
    }
}

static bool dir_is_opposite(uint8_t a, uint8_t b) {
    return (a==0 && b==2) || (a==2 && b==0) || (a==1 && b==3) || (a==3 && b==1);
}

static bool occupied_by_snake(Game *g, int x, int y) {
    for (int i=0;i<MAX_PLAYERS;i++) {
        Player *p = &g->players[i];
        if (!p->used || !p->active || !p->alive) continue;
        for (int k=0;k<(int)p->len;k++) {
            if (p->body[k].x == x && p->body[k].y == y) return true;
        }
    }
    return false;
}

static bool occupied_by_fruit(Game *g, int x, int y) {
    for (int i=0;i<(int)g->num_fruits;i++) {
        if (g->fruits[i].pos.x == x && g->fruits[i].pos.y == y) return true;
    }
    return false;
}

static Cell find_free_cell(Game *g) {
    for (int tries=0;tries<10000;tries++) {
        int x = rand() % g->w;
        int y = rand() % g->h;
        if (is_obstacle(g, x, y)) continue;
        if (occupied_by_snake(g, x, y)) continue;
        if (occupied_by_fruit(g, x, y)) continue;
        return (Cell){(int16_t)x,(int16_t)y};
    }
    for (int y=0;y<g->h;y++) for (int x=0;x<g->w;x++) {
        if (is_obstacle(g, x, y)) continue;
        if (occupied_by_snake(g, x, y)) continue;
        if (occupied_by_fruit(g, x, y)) continue;
        return (Cell){(int16_t)x,(int16_t)y};
    }
    return (Cell){1,1};
}

static int count_active_alive(Game *g) {
    int c=0;
    for (int i=0;i<MAX_PLAYERS;i++) {
        Player *p=&g->players[i];
        if (p->used && p->active && p->alive) c++;
    }
    return c;
}

static void ensure_fruits_count(Game *g) {
    int needed = count_active_alive(g);
    if (needed < 0) needed = 0;
    if (needed > MAX_FRUITS) needed = MAX_FRUITS;

    while (g->num_fruits < (uint8_t)needed) {
        Fruit *f = &g->fruits[g->num_fruits++];
        f->pos = find_free_cell(g);
        f->visited_mask = 0;
    }
    while (g->num_fruits > (uint8_t)needed) {
        g->num_fruits--;
    }
}

static void clear_fruit_visits_for_slot(Game *g, int slot) {
    if (slot < 0 || slot >= MAX_PLAYERS) return;
    uint32_t mask = ~(1u << (uint32_t)slot);
    for (int i = 0; i < g->num_fruits; i++) {
        g->fruits[i].visited_mask &= mask;
    }
}

static void fruit_visit(Game *g, int slot, int x, int y, bool *grew) {
    for (int i = 0; i < (int)g->num_fruits; i++) {
        Fruit *f = &g->fruits[i];
        if (f->pos.x == x && f->pos.y == y) {
            *grew = true;
            g->players[slot].score++;

            f->pos = find_free_cell(g);
            f->visited_mask = 0;

            return;
        }
    }
}

static void kill_player(Player *p) {
    p->alive = false;
    if (p->time_ms_final == 0 && p->spawn_ms != 0) {
    uint64_t now = now_ms();
    uint64_t d = (now > p->spawn_ms) ? (now - p->spawn_ms) : 0;
    if (d > 0xFFFFFFFFULL) d = 0xFFFFFFFFULL;
    p->time_ms_final = (uint32_t)d;
}
}

static void move_snake(Game *g, int slot) {
    Player *p = &g->players[slot];
    if (!p->used || !p->active || !p->alive) return;
    if (p->paused) return;

    if (p->pending_dir != 255) {
        if (!dir_is_opposite(p->dir, p->pending_dir)) p->dir = p->pending_dir;
        p->pending_dir = 255;
    }

    int dx,dy; dir_delta(p->dir, &dx, &dy);
    int nx = p->body[0].x + dx;
    int ny = p->body[0].y + dy;

    if (g->world == 0) {
        if (nx < 0) nx = g->w - 1;
        if (nx >= g->w) nx = 0;
        if (ny < 0) ny = g->h - 1;
        if (ny >= g->h) ny = 0;
    } else {
        if (!in_bounds(g, nx, ny)) { kill_player(p); return; }
    }

    if (is_obstacle(g, nx, ny)) { kill_player(p); return; }
    if (occupied_by_snake(g, nx, ny)) { kill_player(p); return; }

    bool grew = false;
    fruit_visit(g, slot, nx, ny, &grew);

    uint16_t new_len = p->len;
    if (grew) {
        if (new_len < (uint16_t)(sizeof(p->body)/sizeof(p->body[0]))) new_len++;
    }

    for (int i=(int)new_len-1; i>0; i--) p->body[i] = p->body[i-1];
    p->body[0] = (Cell){(int16_t)nx,(int16_t)ny};
    p->len = new_len;
}

static void tick_game(Game *g, uint32_t dt_ms) {
    if (g->global_freeze_ms > 0) {
        if (dt_ms >= g->global_freeze_ms) g->global_freeze_ms = 0;
        else g->global_freeze_ms -= dt_ms;
        return;
    }
    for (int i=0;i<MAX_PLAYERS;i++) move_snake(g, i);
    ensure_fruits_count(g);
}

static bool load_map_file(const char *path, Game *g) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char *lines[2048];
    int line_count = 0;
    int max_w = 0;

    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
        size_t n = strlen(buf);
        while (n>0 && (buf[n-1]=='\n' || buf[n-1]=='\r')) buf[--n] = 0;
        if (n == 0) continue;

        if (line_count >= (int)(sizeof(lines)/sizeof(lines[0]))) break;

        lines[line_count] = strdup(buf);
        if (!lines[line_count]) break;

        if ((int)n > max_w) max_w = (int)n;
        line_count++;
    }
    fclose(f);

    if (line_count <= 0 || max_w <= 0) {
        for (int i=0;i<line_count;i++) free(lines[i]);
        return false;
    }

    g->w = max_w;
    g->h = line_count;

    g->map = (uint8_t*)calloc((size_t)g->w * (size_t)g->h, 1);
    if (!g->map) {
        for (int i=0;i<line_count;i++) free(lines[i]);
        return false;
    }

    for (int y=0;y<g->h;y++) {
        char *ln = lines[y];
        int lw = (int)strlen(ln);
        for (int x=0;x<g->w;x++) {
            char c = (x < lw) ? ln[x] : ' ';
            g->map[idx(g, x, y)] = (c == '#') ? 1 : 0;
        }
        free(lines[y]);
    }

    return true;
}

static void init_player(Player *p, const char *name, Cell spawn, uint16_t keep_score) {
    memset(p, 0, sizeof(*p));
    p->spawn_ms = now_ms();
    p->time_ms_final = 0;
    p->used = true;
    p->connected = true;
    p->active = true;
    p->alive = true;
    p->paused = false;
    p->ready = false;
    p->fd = -1;
    p->dir = 1;
    p->pending_dir = 255;
    p->score = keep_score;
    (void)snprintf(p->name, sizeof(p->name), "%s", (name && name[0]) ? name : "player");
    p->len = 3;
    p->body[0] = spawn;
    p->body[1] = (Cell){(int16_t)(spawn.x-1), spawn.y};
    p->body[2] = (Cell){(int16_t)(spawn.x-2), spawn.y};
}

static int find_player_by_name(Game *g, const char *name) {
    for (int i=0;i<MAX_PLAYERS;i++) {
        if (g->players[i].used && strncmp(g->players[i].name, name, SNAKE_NAME_MAX) == 0) return i;
    }
    return -1;
}

static int alloc_slot(Game *g) {
    for (int i=0;i<MAX_PLAYERS;i++) if (!g->players[i].used) return i;
    return -1;
}

static void build_config_payload(Game *g, uint8_t **out, uint32_t *out_len) {
    uint32_t map_len = (uint32_t)(g->w * g->h);
    uint32_t total = (uint32_t)sizeof(MsgConfig) + map_len;

    uint8_t *buf = (uint8_t*)malloc(total);
    if (!buf) { *out=NULL; *out_len=0; return; }

    MsgConfig cfg;
    cfg.w = htons((uint16_t)g->w);
    cfg.h = htons((uint16_t)g->h);
    cfg.mode = g->mode;
    cfg.world = g->world;
    cfg.time_limit_sec = htons(g->time_limit_sec);
    cfg.map_len = htonl(map_len);

    memcpy(buf, &cfg, sizeof(cfg));
    memcpy(buf + sizeof(cfg), g->map, map_len);

    *out = buf;
    *out_len = total;
}

static void build_state(Game *g, MsgState *st) {
    memset(st, 0, sizeof(*st));
    st->tick_ms = htons((uint16_t)g->tick_ms);
    st->game_over = g->game_over ? 1 : 0;
    st->mode = g->mode;
    st->w = htons((uint16_t)g->w);
    st->h = htons((uint16_t)g->h);
    st->global_freeze_ms = htons(g->global_freeze_ms);
    uint64_t elapsed_ms = now_ms() - g->start_ms;
    st->elapsed_sec = htons((uint16_t)clampi((int)(elapsed_ms / 1000ULL), 0, 65535));

    if (g->mode == 1) {
        uint32_t elapsed_sec = (uint32_t)(elapsed_ms / 1000ULL);
        uint32_t left = (g->time_limit_sec > elapsed_sec) ? (g->time_limit_sec - elapsed_sec) : 0;
        st->time_left_sec = htons((uint16_t)clampi((int)left, 0, 65535));
    } else {
        st->time_left_sec = htons(0);
    }

    uint8_t np = 0;
    for (int i=0;i<MAX_PLAYERS;i++) {
        Player *p = &g->players[i];
        PlayerState *ps = &st->players[i];
        ps->player_id = (uint8_t)i;
        ps->connected = p->connected ? 1 : 0;
        ps->active = p->active ? 1 : 0;
        ps->alive = p->alive ? 1 : 0;
        ps->paused = p->paused ? 1 : 0;
        ps->dir = p->dir;
        ps->score = htons(p->score);

        uint64_t tms;
        if (p->alive) {
          uint64_t now = now_ms();
          tms = (now > p->spawn_ms) ? (now - p->spawn_ms) : 0;
        } else {
          tms = (uint64_t)p->time_ms_final;
        }

ps->time_sec = htons((uint16_t)clampi((int)(tms / 1000ULL), 0, 65535));

        uint16_t send_len = u16min(p->len, (uint16_t)MAX_SEGMENTS);
        ps->len = htons(send_len);
        for (int k=0;k<(int)send_len;k++) ps->body[k] = p->body[k];

        if (p->used) np++;
    }
    st->num_players = np;

    st->num_fruits = g->num_fruits;
    for (int i=0;i<(int)g->num_fruits;i++) {
        st->fruits[i].pos = g->fruits[i].pos;
        st->fruits[i].visited_mask = htonl(g->fruits[i].visited_mask);
    }
}

static bool any_connected_active_alive(Game *g) {
    for (int i=0;i<MAX_PLAYERS;i++) {
        Player *p=&g->players[i];
        if (p->used && p->connected && p->active && p->alive) return true;
    }
    return false;
}

typedef struct {
    int fd;
} ClientCtx;

static Game g_game;

static void *client_thread(void *arg) {
    ClientCtx *c = (ClientCtx*)arg;
    int fd = c->fd;
    free(c);

    uint16_t type=0; uint32_t len=0;
    if (net_recv_header(fd, &type, &len) != 0) goto done;
    if (type != MSG_HELLO || len != sizeof(MsgHello)) goto done;

    MsgHello h;
    if (net_recv_all(fd, &h, (int)sizeof(h)) != 0) goto done;
    h.name[SNAKE_NAME_MAX-1] = 0;

    int slot = -1;

    pthread_mutex_lock(&g_game.mtx);

    if (g_game.game_over) { pthread_mutex_unlock(&g_game.mtx); goto done; }

    int ex = find_player_by_name(&g_game, h.name);
    
    if (ex >= 0 && g_game.players[ex].connected) {
      pthread_mutex_unlock(&g_game.mtx);
      goto done;
    }

    if (ex >= 0) {
        slot = ex;
        g_game.players[slot].connected = true;
        g_game.players[slot].active = true;
        g_game.players[slot].fd = fd;
        if (!g_game.players[slot].alive) {
            uint16_t keep = g_game.players[slot].score;
            Cell sp = find_free_cell(&g_game);
            init_player(&g_game.players[slot], g_game.players[slot].name, sp, keep);
            g_game.players[slot].fd = fd;
            clear_fruit_visits_for_slot(&g_game, slot);
        }
        g_game.global_freeze_ms = 3000;
    } else {
        int s = alloc_slot(&g_game);
        if (s < 0) { pthread_mutex_unlock(&g_game.mtx); goto done; }
        slot = s;
        Cell sp = find_free_cell(&g_game);
        init_player(&g_game.players[slot], h.name, sp, 0);
        g_game.players[slot].fd = fd;
    }

    ensure_fruits_count(&g_game);
    pthread_mutex_unlock(&g_game.mtx);

    MsgWelcome w;
    w.player_id = htonl((uint32_t)slot);
    if (net_send_msg(fd, MSG_WELCOME, &w, (uint32_t)sizeof(w)) != 0) goto done;

    uint8_t *cfg_buf=NULL; uint32_t cfg_len=0;
    pthread_mutex_lock(&g_game.mtx);
    build_config_payload(&g_game, &cfg_buf, &cfg_len);
    pthread_mutex_unlock(&g_game.mtx);

    if (!cfg_buf) goto done;
    if (net_send_msg(fd, MSG_CONFIG, cfg_buf, cfg_len) != 0) { free(cfg_buf); goto done; }
    free(cfg_buf);

    pthread_mutex_lock(&g_game.mtx);
    if (slot >= 0 && slot < MAX_PLAYERS && g_game.players[slot].used) g_game.players[slot].ready = true;
    pthread_mutex_unlock(&g_game.mtx);

    while (g_running) {
        uint16_t t=0; uint32_t l=0;
        if (net_recv_header(fd, &t, &l) != 0) break;

        if (t == MSG_INPUT && l == sizeof(MsgInput)) {
            MsgInput in;
            if (net_recv_all(fd, &in, (int)sizeof(in)) != 0) break;
            pthread_mutex_lock(&g_game.mtx);
            if (slot >= 0 && g_game.players[slot].used && g_game.players[slot].active && g_game.players[slot].alive) {
                if (in.dir <= 3) g_game.players[slot].pending_dir = in.dir;
            }
            pthread_mutex_unlock(&g_game.mtx);
        } else if (t == MSG_PAUSE_TOGGLE && l == 0) {
            pthread_mutex_lock(&g_game.mtx);
            if (slot >= 0 && g_game.players[slot].used && g_game.players[slot].active) {
                bool was = g_game.players[slot].paused;
                g_game.players[slot].paused = !was;
                if (was) g_game.global_freeze_ms = 3000;
            }
            pthread_mutex_unlock(&g_game.mtx);
        } else if (t == MSG_LEAVE && l == 0) {
            pthread_mutex_lock(&g_game.mtx);
            if (slot >= 0 && g_game.players[slot].used) {
                g_game.players[slot].active = false;
                g_game.players[slot].alive = false;
            }
            ensure_fruits_count(&g_game);
            pthread_mutex_unlock(&g_game.mtx);
            break;
        } else {
            if (l > 0) {
                char *tmp = (char*)malloc(l);
                if (!tmp) break;
                if (net_recv_all(fd, tmp, (int)l) != 0) { free(tmp); break; }
                free(tmp);
            }
            if (t == MSG_BYE) break;
        }
    }

done:
    pthread_mutex_lock(&g_game.mtx);
    for (int i=0;i<MAX_PLAYERS;i++) {
        if (g_game.players[i].used && g_game.players[i].fd == fd) {
            g_game.players[i].connected = false;
            g_game.players[i].ready = false;
            g_game.players[i].fd = -1;
            if (!g_game.players[i].active) {
              g_game.players[i].used = false;
              g_game.players[i].name[0] = '\0';
            }

            break;
        }
    }
    pthread_mutex_unlock(&g_game.mtx);

    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    srand((unsigned)time(NULL));
    signal(SIGINT, on_sigint);

    int port = DEFAULT_PORT;
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) port = DEFAULT_PORT;
    }

    const char *map_path = "data/map1.txt";
    if (argc >= 3 && argv[2] && argv[2][0]) map_path = argv[2];

    int mode = 0;
    int world = 1;
    int time_limit = 120;

    if (argc >= 4) mode = atoi(argv[3]);
    if (argc >= 5) world = atoi(argv[4]);
    if (argc >= 6) time_limit = atoi(argv[5]);

    mode = (mode == 1) ? 1 : 0;
    world = (world == 0) ? 0 : 1;
    time_limit = clampi(time_limit, 5, 3600);

    memset(&g_game, 0, sizeof(g_game));
    (void)pthread_mutex_init(&g_game.mtx, NULL);

    g_game.mode = (uint8_t)mode;
    g_game.world = (uint8_t)world;
    g_game.time_limit_sec = (uint16_t)time_limit;
    g_game.tick_ms = 120;
    g_game.global_freeze_ms = 0;
    g_game.last_no_players_ms = 0;
    g_game.game_over = false;
    (void)snprintf(g_game.map_path, sizeof(g_game.map_path), "%s", map_path);

    if (!load_map_file(map_path, &g_game)) {
        fprintf(stderr, "Failed to load map: %s\n", map_path);
        return 1;
    }

    int listen_fd = net_listen_tcp(port);
    if (listen_fd < 0) {
        perror("net_listen_tcp");
        return 1;
    }

    g_game.start_ms = now_ms();
    g_game.last_tick_ms = g_game.start_ms;

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        int sel = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(listen_fd, &rfds)) {
            int cfd = accept(listen_fd, NULL, NULL);
            if (cfd >= 0) {
                ClientCtx *ctx = (ClientCtx*)calloc(1, sizeof(ClientCtx));
                if (ctx) {
                    ctx->fd = cfd;
                    pthread_t th;
                    if (pthread_create(&th, NULL, client_thread, ctx) == 0) {
                        pthread_detach(th);
                    } else {
                        free(ctx);
                        close(cfd);
                    }
                } else {
                    close(cfd);
                }
            }
        }

        uint64_t now = now_ms();
        uint64_t dt64 = now - g_game.last_tick_ms;
        if (dt64 >= (uint64_t)g_game.tick_ms) {
            uint32_t dt = (uint32_t)dt64;
            bool just_finished = false;

            pthread_mutex_lock(&g_game.mtx);

            if (g_game.mode == 1) {
                uint64_t elapsed_ms = now - g_game.start_ms;
                if (elapsed_ms >= (uint64_t)g_game.time_limit_sec * 1000ULL) {
                    g_game.game_over = true;
                    g_running = 0;
                }
            } else {
                if (!any_connected_active_alive(&g_game)) {
                    if (g_game.last_no_players_ms == 0) g_game.last_no_players_ms = now;
                    if (now - g_game.last_no_players_ms >= 10000ULL) {
                        g_game.game_over = true;
                        g_running = 0;
                    }
                } else {
                    g_game.last_no_players_ms = 0;
                }
            }
            
            if (just_finished) {
              for (int i = 0; i < MAX_PLAYERS; i++) {
                Player *p = &g_game.players[i];
                if (!p->used) continue;
                if (p->time_ms_final == 0 && p->spawn_ms != 0) {
                  uint64_t d = (now > p->spawn_ms) ? (now - p->spawn_ms) : 0;
                if (d > 0xFFFFFFFFULL) d = 0xFFFFFFFFULL;
                  p->time_ms_final = (uint32_t)d;
        }
    }
}
            if (g_running) tick_game(&g_game, dt);

            MsgState st;
            build_state(&g_game, &st);

            for (int i=0;i<MAX_PLAYERS;i++) {
                Player *p = &g_game.players[i];
                if (!p->used || !p->connected || !p->ready) continue;
                if (p->fd < 0) continue;
                (void)net_send_msg(p->fd, MSG_STATE, &st, (uint32_t)sizeof(st));
            }

            pthread_mutex_unlock(&g_game.mtx);
            g_game.last_tick_ms = now;
        }

        sleep_ms(5);
    }

    close(listen_fd);
    return 0;
}

