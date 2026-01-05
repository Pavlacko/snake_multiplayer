#define _POSIX_C_SOURCE 200809L
#include "../common/net.h"
#include "../common/protocol.h"

#include <ncurses.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/wait.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 5555
#define SERVER_INFO_FILE "server.info"

/* Utility helpers */

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void fill_player_name(MsgHello *msg, const char *name) {
    memset(msg, 0, sizeof(*msg));
    if (!name) return;

    size_t n = strlen(name);
    if (n >= SNAKE_NAME_MAX) n = SNAKE_NAME_MAX - 1;
    memcpy(msg->name, name, n);
    msg->name[n] = '\0';
}

static void prompt_text(const char *label, char *out, size_t out_size) {
    if (!out || out_size == 0) return;

    out[0] = '\0';

    mvprintw(10, 2, "%s", label);
    clrtoeol();
    refresh();

    echo();
    curs_set(1);

    char buffer[256] = {0};
    getnstr(buffer, (int)sizeof(buffer) - 1);

    noecho();
    curs_set(0);

    snprintf(out, out_size, "%s", buffer);
}

static int prompt_int(const char *label, int default_value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", default_value);

    prompt_text(label, buf, sizeof(buf));
    if (buf[0] == '\0') return default_value;
    return atoi(buf);
}

/* Server discovery */

static int read_server_info(char *host_out, size_t host_size, int *port_out) {
    if (!host_out || !port_out || host_size == 0) return -1;

    FILE *f = fopen(SERVER_INFO_FILE, "r");
    if (!f) return -1;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';

        if (strncmp(line, "HOST=", 5) == 0) {
            size_t len = strlen(line + 5);
            if (len >= host_size) len = host_size - 1;
            memcpy(host_out, line + 5, len);
            host_out[len] = '\0';
        } else if (strncmp(line, "PORT=", 5) == 0) {
            *port_out = atoi(line + 5);
        }
    }

    fclose(f);

    if (*port_out <= 0 || *port_out > 65535) return -1;
    if (host_out[0] == '\0') return -1;

    return 0;
}

/* Server startup */

static int start_server_process(int port) {
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);

        char *argv[] = { "./server/server", port_str, NULL };
        execv(argv[0], argv);
        _exit(127);
    }

    sleep_ms(200);
    return 0;
}

/* Network handshake */

static int perform_handshake(int fd, const char *player_name, int *player_id) {
    MsgHello hello;
    fill_player_name(&hello, player_name);

    if (net_send_msg(fd, MSG_HELLO, &hello, sizeof(hello)) != 0)
        return -1;

    uint16_t type;
    uint32_t len;

    if (net_recv_header(fd, &type, &len) != 0)
        return -1;

    if (type != MSG_WELCOME || len != sizeof(MsgWelcome))
        return -1;

    MsgWelcome welcome;
    if (net_recv_all(fd, &welcome, sizeof(welcome)) != 0)
        return -1;

    *player_id = ntohl(welcome.player_id);
    return 0;
}

/* Client runtime loop */

static void client_main_loop(int fd, int player_id, const char *host, int port) {
    clear();
    mvprintw(1, 2, "Connected to %s:%d", host, port);
    mvprintw(2, 2, "Player ID: %d", player_id);
    mvprintw(4, 2, "Press 'q' to quit.");
    refresh();

    nodelay(stdscr, TRUE);

    while (1) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q')
            break;

        net_send_msg(fd, MSG_PING, NULL, 0);
        sleep_ms(200);
    }

    net_send_msg(fd, MSG_BYE, NULL, 0);
}

/* UI   */

static int startup_menu(void) {
    clear();
    mvprintw(2, 2, "Multiplayer Snake");
    mvprintw(4, 2, "1) Create new game");
    mvprintw(5, 2, "2) Join existing game");
    mvprintw(6, 2, "q) Quit");
    refresh();

    while (1) {
        int ch = getch();
        if (ch == '1') return 1;
        if (ch == '2') return 2;
        if (ch == 'q' || ch == 'Q') return 0;
    }
}

/* main  */

int main(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    int choice = startup_menu();
    if (choice == 0) {
        endwin();
        return 0;
    }

    char player_name[64] = "Player";
    char tmp[64];

    clear();
    mvprintw(2, 2, "Enter player name (default: %s)", player_name);
    refresh();
    prompt_text("Name: ", tmp, sizeof(tmp));
    if (tmp[0] != '\0')
        snprintf(player_name, sizeof(player_name), "%s", tmp);

    char host[128] = DEFAULT_HOST;
    int port = DEFAULT_PORT;

    if (choice == 1) {
        clear();
        mvprintw(2, 2, "Create new game");
        refresh();

        port = prompt_int("Port (default 5555): ", DEFAULT_PORT);
        if (port <= 0 || port > 65535)
            port = DEFAULT_PORT;

        if (start_server_process(port) != 0) {
            endwin();
            fprintf(stderr, "Failed to start server.\n");
            return 1;
        }

    } else {
        if (read_server_info(host, sizeof(host), &port) != 0) {
            clear();
            mvprintw(2, 2, "Join existing game");
            mvprintw(3, 2, "server.info not found, enter manually.");
            refresh();

            prompt_text("Host: ", host, sizeof(host));
            port = prompt_int("Port: ", DEFAULT_PORT);
        }
    }

    int fd = net_connect_tcp(host, port);
    if (fd < 0) {
        endwin();
        fprintf(stderr, "Cannot connect to %s:%d\n", host, port);
        return 1;
    }

    int player_id;
    if (perform_handshake(fd, player_name, &player_id) != 0) {
        close(fd);
        endwin();
        fprintf(stderr, "Handshake failed.\n");
        return 1;
    }

    client_main_loop(fd, player_id, host, port);

    close(fd);
    endwin();
    return 0;
}

