#ifndef HOST_NET_H
#define HOST_NET_H
#include <stddef.h>
#include <stdint.h>
#define HOST_NET_MAX_FRAME 8192u
#define HOST_NET_BUF 16384u

typedef struct {
    int listener;
    int peer;
    unsigned char rx[HOST_NET_BUF];
    size_t rx_len;
    unsigned char tx[HOST_NET_BUF];
    size_t tx_len;
    int ready;
} host_net_t;

typedef void (*host_net_message_cb)(const char *message, void *context);

int host_net_start(host_net_t *n, uint16_t port);
void host_net_close(host_net_t *n);
int host_net_listener_fd(const host_net_t *n);
int host_net_peer_fd(const host_net_t *n);
short host_net_peer_events(const host_net_t *n);
void host_net_accept(host_net_t *n);
void host_net_io(host_net_t *n, short revents, host_net_message_cb cb, void *ctx);
int host_net_send(host_net_t *n, const char *message);
int host_net_connected(const host_net_t *n);
#endif
