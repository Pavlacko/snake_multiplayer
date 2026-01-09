#pragma once
#include <stdint.h>

int net_send_all(int fd, const void *buf, int len);
int net_recv_all(int fd, void *buf, int len);

int net_send_msg(int fd, uint16_t type, const void *payload, uint32_t len);
int net_recv_header(int fd, uint16_t *type, uint32_t *len);

int net_connect_tcp(const char *host, int port);
int net_listen_tcp(int port);
