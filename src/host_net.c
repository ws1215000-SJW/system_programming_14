#define _POSIX_C_SOURCE 200809L
#include "host_net.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
static void drop_peer(host_net_t *n) {
    if (n->peer >= 0) close(n->peer);
    n->peer = -1;
    n->rx_len = n->tx_len = 0;
    n->ready = 0;
}
int host_net_start(host_net_t *n, uint16_t port) {
    memset(n, 0, sizeof(*n));
    n->listener = n->peer = -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* local control client */
    if (nonblock(fd) < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
        listen(fd, 1) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    n->listener = fd;
    return 0;
}
void host_net_close(host_net_t *n) {
    drop_peer(n);
    if (n->listener >= 0) close(n->listener);
    n->listener = -1;
}
int host_net_listener_fd(const host_net_t *n) { return n->listener; }
int host_net_peer_fd(const host_net_t *n) { return n->peer; }
short host_net_peer_events(const host_net_t *n) {
    return POLLIN | (n->tx_len ? POLLOUT : 0);
}
int host_net_connected(const host_net_t *n) { return n->peer >= 0; }
void host_net_accept(host_net_t *n) {
    if (n->listener < 0) return;
    for (;;) {
        int fd = accept(n->listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (n->peer >= 0 || nonblock(fd) < 0) {
            close(fd);
            continue;
        }
        n->peer = fd;
        n->ready = 0;
        n->rx_len = n->tx_len = 0;
        fprintf(stderr, "[host-net] control connected\n");
    }
}
int host_net_send(host_net_t *n, const char *message) {
    if (n->peer < 0 || !message) return -1;
    size_t len = strlen(message);
    if (len > HOST_NET_MAX_FRAME || len + 4 > HOST_NET_BUF - n->tx_len) return -1;
    uint32_t wire_len = htonl((uint32_t)len);
    memcpy(n->tx + n->tx_len, &wire_len, 4);
    n->tx_len += 4;
    memcpy(n->tx + n->tx_len, message, len);
    n->tx_len += len;
    return 0;
}
static void flush_tx(host_net_t *n) {
    while (n->tx_len) {
        ssize_t k = send(n->peer, n->tx, n->tx_len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (k > 0) {
            memmove(n->tx, n->tx + k, n->tx_len - (size_t)k);
            n->tx_len -= (size_t)k;
        } else if (k < 0 && errno == EINTR) {
            continue;
        } else if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            drop_peer(n);
            break;
        }
    }
}
void host_net_io(host_net_t *n, short revents, host_net_message_cb cb, void *ctx) {
    if (n->peer < 0) return;
    if (revents & (POLLERR | POLLNVAL | POLLHUP)) {
        drop_peer(n);
        return;
    }
    if (revents & POLLIN) {
        for (;;) {
            if (n->rx_len == HOST_NET_BUF) { drop_peer(n); return; }
            ssize_t k = recv(n->peer, n->rx + n->rx_len,
                             HOST_NET_BUF - n->rx_len, MSG_DONTWAIT);
            if (k > 0) n->rx_len += (size_t)k;
            else if (k == 0) { drop_peer(n); return; }
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else { drop_peer(n); return; }
        }
        while (n->rx_len >= 4) {
            uint32_t wire_len;
            memcpy(&wire_len, n->rx, 4);
            size_t len = ntohl(wire_len);
            if (len > HOST_NET_MAX_FRAME) { drop_peer(n); return; }
            if (n->rx_len < 4 + len) break;
            char msg[HOST_NET_MAX_FRAME + 1];
            memcpy(msg, n->rx + 4, len);
            msg[len] = '\0';
            memmove(n->rx, n->rx + 4 + len, n->rx_len - 4 - len);
            n->rx_len -= 4 + len;
            if (cb) cb(msg, ctx);
            if (n->peer < 0) return;
        }
    }
    if (n->peer >= 0 && ((revents & POLLOUT) || n->tx_len)) flush_tx(n);
}
