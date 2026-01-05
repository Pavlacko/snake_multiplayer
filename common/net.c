#define _POSIX_C_SOURCE 200809L

#include "net.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int net_send_all(int fd, const void *buf, int len) {
    const char *p = (const char *)buf;
    int sent = 0;
    while (sent < len) {
        int n = (int)send(fd, p + sent, (size_t)(len - sent), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += n;
    }
    return 0;
}

int net_recv_all(int fd, void *buf, int len) {
    char *p = (char *)buf;
    int recvd = 0;
    while (recvd < len) {
        int n = (int)recv(fd, p + recvd, (size_t)(len - recvd), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; // peer closed
        recvd += n;
    }
    return 0;
}

int net_send_msg(int fd, uint16_t type, const void *payload, uint32_t len) {
    MsgHeader h;
    h.type = htons(type);
    h.len  = htonl(len);

    if (net_send_all(fd, &h, (int)sizeof(h)) != 0) return -1;
    if (len > 0 && payload != NULL) {
        if (net_send_all(fd, payload, (int)len) != 0) return -1;
    }
    return 0;
}

int net_recv_header(int fd, uint16_t *type, uint32_t *len) {
    MsgHeader h;
    if (net_recv_all(fd, &h, (int)sizeof(h)) != 0) return -1;

    *type = ntohs(h.type);
    *len  = ntohl(h.len);
    return 0;
}

int net_connect_tcp(const char *host, int port) {
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;

        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            freeaddrinfo(res);
            return fd;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return -1;
}

int net_listen_tcp(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

