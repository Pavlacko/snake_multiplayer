#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>

int  net_send_all(int fd, const void *buf, size_t len);
int  net_recv_all(int fd, void *buf, size_t len);

int  net_send_msg(int fd, uint16_t type, const void *payload, uint32_t len);
int  net_recv_header(int fd, uint16_t *type, uint32_t *len);

int  net_listen_tcp(uint16_t port);
int  net_connect_tcp(const char *host, uint16_t port);

#endif
