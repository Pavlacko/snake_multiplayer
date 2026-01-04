#define _POSIX_C_SOURCE 200809L
#include "../common/net.h"
#include "../common/protocol.h"

#include <ncurses.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define HOST "127.0.0.1"
#define PORT 5555

static void fill_name(MsgHello *h, const char *name) {
    memset(h, 0, sizeof(*h));
    size_t n = strlen(name);
    if (n > SNAKE_NAME_MAX) n = SNAKE_NAME_MAX;
    memcpy(h->name, name, n);
}

int main(void) {
    int fd = net_connect_tcp(HOST, PORT);
    if (fd < 0) return 1;

    MsgHello hello;
    fill_name(&hello, "Pavlacko");
    if (net_send_msg(fd, MSG_HELLO, &hello, sizeof(hello)) != 0) {
        close(fd);
        return 1;
    }

    uint16_t type = 0;
    uint32_t len = 0;
    if (net_recv_header(fd, &type, &len) != 0 || type != MSG_WELCOME || len != sizeof(MsgWelcome)) {
        close(fd);
        return 1;
    }

    MsgWelcome w;
    if (net_recv_all(fd, &w, sizeof(w)) != 0) {
        close(fd);
        return 1;
    }

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    mvprintw(1, 2, "Connected to %s:%d", HOST, PORT);
    mvprintw(2, 2, "player_id = %d", w.player_id);
    mvprintw(4, 2, "Press 'q' to quit. (Block A handshake test)");
    refresh();

    while (1) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q') break;

        (void)net_send_msg(fd, MSG_PING, NULL, 0);
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 200 * 1000 * 1000;
        nanosleep(&ts, NULL);
    }

    endwin();
    (void)net_send_msg(fd, MSG_BYE, NULL, 0);
    close(fd);
    return 0;
}
