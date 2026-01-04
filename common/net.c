#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE

#include "net.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int net_send_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

int net_recv_all(int fd, void *buf, size_t len) {
    unsigned char *p = (unsigned char *)buf;
    size_t recvd = 0;
    while (recvd < len) {
        ssize_t n = recv(fd, p + recvd, len - recvd, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        recvd += (size_t)n;
    }
    return 0;
}

int net_send_msg(int fd, uint16_t type, const void *payload, uint32_t len) {
    MsgHeader h;
    h.magic   = htonl(SNAKE_PROTO_MAGIC);
    h.version = htons(SNAKE_PROTO_VER);
    h.type    = htons(type);
    h.len     = htonl(len);

    if (net_send_all(fd, &h, sizeof(h)) != 0) return -1;
    if (len > 0 && payload) {
        if (net_send_all(fd, payload, len) != 0) return -1;
    }
    return 0;
}

int net_recv_header(int fd, uint16_t *type, uint32_t *len) {
    MsgHeader h;
    if (net_recv_all(fd, &h, sizeof(h)) != 0) return -1;

    if (ntohl(h.magic) != SNAKE_PROTO_MAGIC) return -1;
    if (ntohs(h.version) != SNAKE_PROTO_VER) return -1;

    if (type) *type = ntohs(h.type);
    if (len)  *len  = ntohl(h.len);
    return 0;
}

int net_listen_tcp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int net_connect_tcp(const char *host, uint16_t port) {
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;

    int fd = -1;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;

        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
            break;

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}
