#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>

int run_server(
    int port,
    const char *map_path,
    int mode,
    int world,
    int time_limit,
    int w,
    int h
);

void on_sigint(int sig);

#endif
