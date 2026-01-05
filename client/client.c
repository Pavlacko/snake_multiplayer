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
#include <sys/select.h>
#include <unistd.h>
#include <time.h>

#define DEFAULT_PORT 5555
#define SERVER_INFO_FILE "server.info"

static volatile sig_atomic_t g_running = 1;

static void on_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int parse_port(const char *s, int def_port) {
    if (!s || !s[0]) return def_port;
    int p = atoi(s);
    if (p <= 0 || p > 65535) return def_port;
    return p;
}

static int prompt_str(int y, int x, const char *label, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return -1;

    out[0] = '\0';

    mvprintw(y, x, "%s", label ? label : "");
    clrtoeol();
    refresh();

    echo();
    curs_set(1);

    size_t len = 0;

    for (;;) {
        int ch = getch();

        if (ch == 27) {                 
            noecho();
            curs_set(0);
            out[0] = '\0';
            return 1;                 
        }

        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            break;                  
        }

        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len > 0) {
                len--;
                out[len] = '\0';
                int px = x + (int)strlen(label) + (int)len;
                mvaddch(y, px, ' ');
                move(y, px);
                refresh();
            }
            continue;
        }

        if (ch == ERR) continue;
        if (ch >= 32 && ch <= 126) {    
            if (len + 1 < out_sz) {
                out[len++] = (char)ch;
                out[len] = '\0';
                mvaddch(y, x + (int)strlen(label) + (int)len - 1, ch);
                refresh();
            }
        }
    }

    noecho();
    curs_set(0);
    return 0;                           
}

static void fill_name(MsgHello *h, const char *name) {
    memset(h, 0, sizeof(*h));
    if (!name) return;
    size_t n = strlen(name);
    if (n > SNAKE_NAME_MAX) n = SNAKE_NAME_MAX;
    memcpy(h->name, name, n);
}

static int do_handshake(int fd, const char *player_name, int *player_id_out) {
    if (!player_id_out) return -1;

    MsgHello hello;
    fill_name(&hello, player_name);

    if (net_send_msg(fd, MSG_HELLO, &hello, (uint32_t)sizeof(hello)) != 0) return -1;

    uint16_t type = 0;
    uint32_t len = 0;
    if (net_recv_header(fd, &type, &len) != 0) return -1;
    if (type != MSG_WELCOME || len != (uint32_t)sizeof(MsgWelcome)) return -1;

    MsgWelcome w;
    if (net_recv_all(fd, &w, (int)sizeof(w)) != 0) return -1;

    *player_id_out = (int)ntohl((uint32_t)w.player_id);
    return 0;
}

static int start_server_process(int port) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        signal(SIGHUP, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
        (void)setsid();

        char portstr[16];
        snprintf(portstr, sizeof(portstr), "%d", port);

        char *argv[] = { "./server/server", portstr, NULL };
        execv(argv[0], argv);
        _exit(127);
    }
    return 0;
}

static void draw_status(const char *title, const char *line1, const char *line2) {
    clear();
    mvprintw(1, 2, "%s", title ? title : "");
    mvprintw(3, 2, "%s", line1 ? line1 : "");
    mvprintw(4, 2, "%s", line2 ? line2 : "");
    mvprintw(6, 2, "q = quit");
    refresh();
}

static int run_session(const char *host_in, int port_in, const char *player_name)
{
    char host[128];
    snprintf(host, sizeof(host), "%s", host_in ? host_in : "127.0.0.1");
    int port = port_in > 0 ? port_in : DEFAULT_PORT;

    if (!player_name || player_name[0] == '\0') return -1;

    int fd = net_connect_tcp(host, port);
    if (fd < 0) {
        draw_status("Error", "Connect failed", "Press any key...");
        timeout(-1);
        getch();
        timeout(0);
        return -1;
    }

    int player_id = -1;
    if (do_handshake(fd, player_name, &player_id) != 0) {
        close(fd);
        draw_status("Error", "Handshake failed", "Press any key...");
        timeout(-1);
        getch();
        timeout(0);
        return -1;
    }

    g_running = 1;
    timeout(0);

    long tick_ms = 0;
    long ping_acc = 0;

    while (g_running) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q') {
            (void)net_send_msg(fd, MSG_BYE, NULL, 0);
            break;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;

        int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(fd, &rfds)) {
            uint16_t type = 0;
            uint32_t len = 0;
            if (net_recv_header(fd, &type, &len) != 0) {
                draw_status("Disconnected", "recv header failed", "Press any key...");
                timeout(-1);
                getch();
                timeout(0);
                break;
            }

            if (len > 0) {
                char *tmp = (char *)malloc(len);
                if (!tmp) {
                    draw_status("Error", "Out of memory", "Press any key...");
                    timeout(-1);
                    getch();
                    timeout(0);
                    break;
                }
                if (net_recv_all(fd, tmp, (int)len) != 0) {
                    free(tmp);
                    draw_status("Disconnected", "recv payload failed", "Press any key...");
                    timeout(-1);
                    getch();
                    timeout(0);
                    break;
                }
                free(tmp);
            }

            if (type == MSG_BYE) {
                draw_status("Disconnected", "Server sent BYE", "Press any key...");
                timeout(-1);
                getch();
                timeout(0);
                break;
            }
        }

        ping_acc += 50;
        tick_ms += 50;

        if (ping_acc >= 200) {
            (void)net_send_msg(fd, MSG_PING, NULL, 0);
            ping_acc = 0;
        }

        char l1[256];
        char l2[256];
        snprintf(l1, sizeof(l1), "Connected to %.100s:%d", host, port);
        snprintf(l2, sizeof(l2), "name=%.32s  player_id=%d  time=%ldms", player_name, player_id, tick_ms);
        draw_status("Session", l1, l2);
    }

    close(fd);
    return 0;
}


static int menu(void) {
    clear();
    mvprintw(1, 2, "Multiplayer Snake");
    mvprintw(3, 2, "1) Create new game");
    mvprintw(4, 2, "2) Join existing game");
    mvprintw(5, 2, "q) Quit");
    refresh();
    nodelay(stdscr, FALSE);
    int ch = getch();
    nodelay(stdscr, TRUE);
    return ch;
}

static void create_new_game(void)
{
    char name[64];

    clear();
    mvprintw(1, 2, "Create new game");

    prompt_str(3, 2, "Player name: ", name, sizeof(name));

    if (name[0] == '\0') {
        draw_status("Error", "Name is required", "Press any key...");
        timeout(-1);
        getch();
        timeout(0);
        return;
    }

    int port = DEFAULT_PORT;

    if (start_server_process(port) != 0) {
        draw_status("Error", "Failed to start server", "Press any key...");
        timeout(-1);
        getch();
        timeout(0);
        return;
    }

    sleep_ms(200);

    (void)run_session("127.0.0.1", port, name);
}

static void join_existing_game(void)
{
    char name[64];
    char host[128];
    char port_s[32];

    for (;;) {
        clear();
        mvprintw(1, 2, "Join existing game (ESC = back)");

        if (prompt_str(3, 2, "Player name: ", name, sizeof(name)) == 1) return;
        if (prompt_str(5, 2, "Host: ", host, sizeof(host)) == 1) return;
        if (prompt_str(7, 2, "Port: ", port_s, sizeof(port_s)) == 1) return;

        if (name[0] == '\0' || host[0] == '\0' || port_s[0] == '\0') {
            draw_status("Error", "All fields are required", "Press any key...");
            timeout(-1); getch(); timeout(0);
            continue;
        }

        int port = parse_port(port_s, 0);
        if (port <= 0) {
            draw_status("Error", "Invalid port", "Press any key...");
            timeout(-1); getch(); timeout(0);
            continue;
        }

        (void)run_session(host, port, name);
        return;
    }
}

int main(void) {
    signal(SIGINT, on_sigint);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    while (1) {
        int ch = menu();
        if (ch == '1') {
            create_new_game();
        } else if (ch == '2') {
            join_existing_game();
        } else if (ch == 'q' || ch == 'Q') {
            break;
        }
    }

    endwin();
    return 0;
}



