/*
 * mesh_net.c - Multi-peer mesh networking, simulated over UDP.
 *
 * Real hardware would use ESP-NOW (a connectionless Wi-Fi protocol)
 * for the radio layer. Since we're running as host processes without
 * radios, UDP sockets on localhost stand in for "nodes talking over
 * the air": each node binds to its own port and sends datagrams
 * directly to its known peers' ports, which mirrors ESP-NOW's
 * peer-list broadcast model closely enough to keep the higher-level
 * node/state-machine logic identical if this is ever ported.
 */
#include "hermes.h"
#include <string.h>
#include <stdio.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>

bool mesh_node_init(mesh_node_t *node, uint16_t local_port) {
    memset(node, 0, sizeof(*node));
    node->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (node->sockfd < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(local_port);

    if (bind(node->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(node->sockfd);
        return false;
    }

    node->local_port = local_port;
    node->peer_count = 0;
    return true;
}

bool mesh_node_add_peer(mesh_node_t *node, uint16_t peer_port) {
    if (node->peer_count >= MESH_MAX_PEERS) return false;
    node->peer_ports[node->peer_count++] = peer_port;
    return true;
}

bool mesh_node_send(mesh_node_t *node, const uint8_t *data, size_t len) {
    if (len > MESH_MAX_PACKET) return false;

    bool all_ok = true;
    for (int i = 0; i < node->peer_count; i++) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(node->peer_ports[i]);

        ssize_t sent = sendto(node->sockfd, data, len, 0,
                               (struct sockaddr *)&addr, sizeof(addr));
        if (sent < 0 || (size_t)sent != len) all_ok = false;
    }
    return all_ok;
}

int mesh_node_recv(mesh_node_t *node, uint8_t *buf, size_t buf_cap,
                    int timeout_ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(node->sockfd, &fds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ready = select(node->sockfd + 1, &fds, NULL, NULL, &tv);
    if (ready < 0) return -1;
    if (ready == 0) return 0; /* timeout */

    ssize_t n = recvfrom(node->sockfd, buf, buf_cap, 0, NULL, NULL);
    if (n < 0) return -1;
    return (int)n;
}

void mesh_node_close(mesh_node_t *node) {
    if (node->sockfd >= 0) {
        close(node->sockfd);
        node->sockfd = -1;
    }
}
