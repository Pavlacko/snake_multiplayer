#define _POSIX_C_SOURCE 200809L

#include "../common/net.h"
#include "../common/protocol.h"

#include <arpa/inet.h>
#include <ncurses.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "client.h"

static volatile sig_atomic_t g_running = 1;

static void on_sigint(int sig) { (void)sig; g_running = 0; }

static void trim_nl(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) { s[n-1] = 0; n--; }
}

static void read_line(const char *prompt, char *out, size_t cap, const char *def) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (prompt) {
        printf("%s", prompt);
        fflush(stdout);
    }
    if (fgets(out, (int)cap, stdin) == NULL) {
        out[0] = 0;
        return;
    }
    trim_nl(out);
    if (out[0] == 0 && def) (void)snprintf(out, cap, "%s", def);
}

static int spawn_server_process(int port, const char *map_path, int mode, int world, int time_limit, int w, int h) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char pbuf[16], mbuf[16], worldbuf[16], tbuf[16], wdim[16], hdim[16];
        (void)snprintf(pbuf, sizeof(pbuf), "%d", port);
        (void)snprintf(mbuf, sizeof(mbuf), "%d", mode);
        (void)snprintf(worldbuf, sizeof(worldbuf), "%d", world);
        (void)snprintf(tbuf, sizeof(tbuf), "%d", time_limit);
        (void)snprintf(wdim, sizeof(wdim), "%d", w);
        (void)snprintf(hdim, sizeof(hdim), "%d", h);

        char *argvv[10];
        int i = 0;

        argvv[i++] = (char *)"./server/server";
        argvv[i++] = pbuf;
        argvv[i++] = (char *)(map_path ? map_path : "data/map1.txt");
        argvv[i++] = mbuf;
        argvv[i++] = worldbuf;
        argvv[i++] = tbuf;

        if (map_path && strcmp(map_path, "-") == 0) {
          argvv[i++] = wdim;
          argvv[i++] = hdim;
        }

        argvv[i++] = NULL;

        execv(argvv[0], argvv);
        _exit(127);
    }
    return 0; 
}

static int connect_and_handshake(const char *host, int port, const char *name, int *out_fd, int *out_player_id, MsgConfig *out_cfg, uint8_t **out_map) {
    int fd = net_connect_tcp(host, port);
    if (fd < 0) return -1;

    MsgHello h;
    memset(&h, 0, sizeof(h));
    (void)snprintf(h.name, sizeof(h.name), "%s", (name && name[0]) ? name : "player");
    if (net_send_msg(fd, MSG_HELLO, &h, (uint32_t)sizeof(h)) != 0) { close(fd); return -1; }

    uint16_t t=0; uint32_t l=0;
    if (net_recv_header(fd, &t, &l) != 0 || t != MSG_WELCOME || l != sizeof(MsgWelcome)) { close(fd); return -1; }
    MsgWelcome w;
    if (net_recv_all(fd, &w, (int)sizeof(w)) != 0) { close(fd); return -1; }
    int pid = (int)ntohl(w.player_id);

    if (net_recv_header(fd, &t, &l) != 0 || t != MSG_CONFIG || l < sizeof(MsgConfig)) { close(fd); return -1; }

    uint8_t *buf = (uint8_t*)malloc(l);
    if (!buf) { close(fd); return -1; }
    if (net_recv_all(fd, buf, (int)l) != 0) { free(buf); close(fd); return -1; }

    MsgConfig cfg;
    memcpy(&cfg, buf, sizeof(cfg));

    uint16_t W = ntohs(cfg.w);
    uint16_t H = ntohs(cfg.h);
    uint32_t map_len = ntohl(cfg.map_len);
    if (map_len != (uint32_t)W * (uint32_t)H) { free(buf); close(fd); return -1; }
    if (sizeof(MsgConfig) + map_len != l) { free(buf); close(fd); return -1; }

    uint8_t *map = (uint8_t*)malloc(map_len);
    if (!map) { free(buf); close(fd); return -1; }
    memcpy(map, buf + sizeof(MsgConfig), map_len);
    free(buf);

    if (out_fd) *out_fd = fd;
    if (out_player_id) *out_player_id = pid;
    if (out_cfg) *out_cfg = cfg;
    if (out_map) *out_map = map;
    else free(map);

    return 0;
}

static void draw_game(const MsgState *st, const uint8_t *map, int my_id) {
    int W = (int)ntohs(st->w);
    int H = (int)ntohs(st->h);

    erase();

    for (int y=0;y<H;y++) {
        for (int x=0;x<W;x++) {
            char ch = ' ';
            if (map && map[y*W + x] == 1) ch = '#';
            mvaddch(y, x, ch);
        }
    }

    int nf = (int)st->num_fruits;
    if (nf > MAX_FRUITS) nf = MAX_FRUITS;
    for (int i=0;i<nf;i++) {
        int fx = st->fruits[i].pos.x;
        int fy = st->fruits[i].pos.y;
        if (fx>=0 && fx<W && fy>=0 && fy<H) mvaddch(fy, fx, '*');
    }

    for (int i=0;i<MAX_PLAYERS;i++) {
        const PlayerState *ps = &st->players[i];
        if (!ps->active || !ps->alive) continue;

        int len = (int)ntohs(ps->len);
        if (len > MAX_SEGMENTS) len = MAX_SEGMENTS;
        for (int k=0;k<len;k++) {
            int sx = ps->body[k].x;
            int sy = ps->body[k].y;
            if (sx<0 || sx>=W || sy<0 || sy>=H) continue;
            char c = (k==0) ? (i==my_id ? '@' : 'O') : (i==my_id ? 'o' : 'x');
            mvaddch(sy, sx, c);
        }
    }

    int hud_y = H + 1;
    mvprintw(hud_y, 0, "WASD/Arrows=move | P=pause | Q=leave");
    mvprintw(hud_y+1, 0, "Mode=%s | Freeze=%dms | GameOver=%d",
             st->mode ? "TIME" : "STANDARD",
             (int)ntohs(st->global_freeze_ms),
             (int)st->game_over);
    
    mvprintw(hud_y+2, 0, "Elapsed: %us", (unsigned)ntohs(st->elapsed_sec));

    if (st->mode == 1) {
    mvprintw(hud_y+3, 0, "Remaining: %us", (unsigned)ntohs(st->time_left_sec));
    }

    int row = hud_y + (st->mode == 1 ? 4 : 3);

    mvprintw(row++, 0, "Scores:");
      for (int i = 0; i < MAX_PLAYERS; i++) {
        const PlayerState *ps = &st->players[i];
        if (!ps->connected) continue;

    mvprintw(row++, 0, "P%d score=%u time=%us %s%s", i, (unsigned)ntohs(ps->score), (unsigned)ntohs(ps->time_sec), ps->alive ? "" : "DEAD ",ps->paused ? "PAUSED" : "");

    refresh();
}}

static uint8_t key_to_dir(int ch) {
    switch (ch) {
        case KEY_UP: return 0;
        case KEY_RIGHT: return 1;
        case KEY_DOWN: return 2;
        case KEY_LEFT: return 3;
        case 'w': case 'W': return 0;
        case 'd': case 'D': return 1;
        case 's': case 'S': return 2;
        case 'a': case 'A': return 3;
        default: return 255;
    }
}

int run_game_session(const char *host, int port, const char *name) {
    int fd = -1, my_id = -1;
    MsgConfig cfg;
    uint8_t *map = NULL;

    if (connect_and_handshake(host, port, name, &fd, &my_id, &cfg, &map) != 0) {
        printf("Connect/handshake failed.\n");
        return 1;
    }

    uint16_t W = ntohs(cfg.w);
    uint16_t H = ntohs(cfg.h);
    uint32_t map_len = ntohl(cfg.map_len);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    bool local_running = true;

    while (g_running && local_running) {
        int ch = getch();
        if (ch != ERR) {
            if (ch == 27 || ch == 'q' || ch == 'Q') {
                (void)net_send_msg(fd, MSG_LEAVE, NULL, 0);
                local_running = false;
            } else if (ch == 'p' || ch == 'P') {
                (void)net_send_msg(fd, MSG_PAUSE_TOGGLE, NULL, 0);
            } else {
                uint8_t d = key_to_dir(ch);
                if (d != 255) {
                    MsgInput in; in.dir = d;
                    (void)net_send_msg(fd, MSG_INPUT, &in, (uint32_t)sizeof(in));
                }
            }
        }

        uint16_t t=0; uint32_t l=0;
        if (net_recv_header(fd, &t, &l) != 0) break;

        if (t == MSG_STATE && l == sizeof(MsgState)) {
            MsgState st;
            if (net_recv_all(fd, &st, (int)sizeof(st)) != 0) break;
            draw_game(&st, map, my_id);

        if (st.game_over) {
            nodelay(stdscr, FALSE);
            clear();
            int row = 0;

        mvprintw(row++, 0, "=== GAME OVER ===");
        mvprintw(row++, 0, "Elapsed: %us", (unsigned)ntohs(st.elapsed_sec));
        if (st.mode == 1) mvprintw(row++, 0, "Time limit reached.");

        row++;
        mvprintw(row++, 0, "Results:");
        mvprintw(row++, 0, "----------------------------------------");
        mvprintw(row++, 0, "%-6s %8s %10s", "PLAYER", "SCORE", "TIME(s)");
        mvprintw(row++, 0, "----------------------------------------");

        for (int i = 0; i < MAX_PLAYERS; i++) {
          PlayerState *ps = &st.players[i];

          if (!ps->connected && !ps->active && ntohs(ps->len) == 0 && ntohs(ps->score) == 0) continue;
          char player_label[8];
          snprintf(player_label, sizeof(player_label), "P%d", i);
          mvprintw(row++, 0, "%-6s %8u %8u", player_label,  (unsigned)ntohs(ps->score), (unsigned)ntohs(ps->time_sec));
    }

        row++;
        mvprintw(row++, 0, "Press any key to return to menu...");
        refresh();
        getch();

        nodelay(stdscr, TRUE);

        local_running = false;
        continue;
      }
        if (st.game_over) {
            napms(1200);
            local_running = false;
        }
        } else {
            if (l > 0) {
                uint8_t *tmp = (uint8_t*)malloc(l);
                if (!tmp) break;
                if (net_recv_all(fd, tmp, (int)l) != 0) { free(tmp); break; }
                free(tmp);
            }
            if (t == MSG_BYE) break;
        }
    }

    endwin();
    close(fd);
    free(map);
    (void)map_len;
    (void)W;
    (void)H;
    return 0;
}

int main(void) {
    signal(SIGINT, on_sigint);

    while (g_running) {
        printf("\n=== Multiplayer Snake ===\n");
        printf("1) New game\n");
        printf("2) Connect\n");
        printf("3) Quit\n");

        char choice_s[32];
        read_line("Option: ", choice_s, sizeof(choice_s), "3");
        int choice = atoi(choice_s);

        if (choice == 3) break;

        if (choice == 1) {
            char name[SNAKE_NAME_MAX];
            char port_s[32];
            char mode_s[32];
            char world_s[32];
            char time_s[32];

            read_line("Port (exmp. 5555): ", port_s, sizeof(port_s), "5555");
            read_line("Mode (0=standard,1=time): ", mode_s, sizeof(mode_s), "0");
            read_line("World (0=wrap, without obstacles,1=obstacles, no wrap): ", world_s, sizeof(world_s), "0");
            read_line("Time limit sec (only mode=1): ", time_s, sizeof(time_s), "120");

            int port = atoi(port_s);
            int mode = atoi(mode_s);
            int world = atoi(world_s);
            int time_limit = atoi(time_s);
            
            int w = 40, h = 20;
            if (world == 0) {
              char w_s[16], h_s[16];
              read_line("Map width (exmp 40): ", w_s, sizeof(w_s), "40");
              read_line("Map height (exmp 20): ", h_s, sizeof(h_s), "20");
              w = atoi(w_s);
              h = atoi(h_s);
              if (w < 10) w = 10;
              if (h < 10) h = 10;
            }
            
            if (port <= 0 || port > 65535) { printf("Incorrect port.\n"); continue; }
            mode = (mode == 1) ? 1 : 0;
            world = (world == 0) ? 0 : 1;
            time_limit = (time_limit <= 0) ? 120 : time_limit;

            const char *map_path = (world == 0) ? "-" : "data/map1.txt";  

            if (spawn_server_process(port, map_path, mode, world, time_limit, w, h) != 0) {
              printf("Failed to start the server.\n");
              continue;
            }

            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 200 * 1000000L;
            nanosleep(&ts, NULL);

            (void)run_game_session("127.0.0.1", port, name);

        } else if (choice == 2) {
            char name[SNAKE_NAME_MAX];
            char host[128];
            char port_s[32];

            read_line("Host: ", host, sizeof(host), "127.0.0.1");
            read_line("Port: ", port_s, sizeof(port_s), "5555");

            int port = atoi(port_s);
            if (port <= 0 || port > 65535) { printf("Wrong input.\n"); continue; }

            (void)run_game_session(host, port, name);
        } else {
            printf("Wrong option.\n");
        }
    }

    return 0;
}

