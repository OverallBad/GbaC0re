/* GbaC0re link-cable bridge: portable UDP protocol. See link_proto.h. */
#include "link_proto.h"

#include <string.h>

#if defined(LINK_PS5)
/* PS5 payload: no system headers here. The backend lives in link_ps5.c and
 * is installed at boot via link_ps5_install(). */
#define LINK_PS5_BACKEND 1
#elif defined(_WIN32)
#define LINK_WINSOCK 1
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#define LINK_POSIX 1
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#endif

/* ------------------------------------------------------------ packets --- */

#define LINK_PKT_LEN 16

static void pkt_build(uint8_t *p, uint8_t type, uint8_t mode,
                      uint32_t seq, uint32_t data) {
    p[0] = LINK_MAGIC0; p[1] = LINK_MAGIC1;
    p[2] = LINK_MAGIC2; p[3] = LINK_MAGIC3;
    p[4] = type; p[5] = mode;
    p[6] = (uint8_t)(seq);        p[7] = (uint8_t)(seq >> 8);
    p[8] = (uint8_t)(seq >> 16);  p[9] = (uint8_t)(seq >> 24);
    p[10] = (uint8_t)(data);      p[11] = (uint8_t)(data >> 8);
    p[12] = (uint8_t)(data >> 16); p[13] = (uint8_t)(data >> 24);
    p[14] = 0; p[15] = 0;
}

static int pkt_parse(const uint8_t *p, unsigned len, uint8_t *type,
                     uint8_t *mode, uint32_t *seq, uint32_t *data) {
    if (len < LINK_PKT_LEN) return 0;
    if (p[0] != LINK_MAGIC0 || p[1] != LINK_MAGIC1 ||
        p[2] != LINK_MAGIC2 || p[3] != LINK_MAGIC3) return 0;
    *type = p[4]; *mode = p[5];
    *seq  = (uint32_t)p[6] | ((uint32_t)p[7] << 8) |
            ((uint32_t)p[8] << 16) | ((uint32_t)p[9] << 24);
    *data = (uint32_t)p[10] | ((uint32_t)p[11] << 8) |
            ((uint32_t)p[12] << 16) | ((uint32_t)p[13] << 24);
    return 1;
}

/* -------------------------------------------------- built-in backends --- */

#if LINK_WINSOCK

static int ws_started = 0;

static link_sock_t ws_create_udp(void) {
    if (!ws_started) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return LINK_SOCK_INVALID;
        ws_started = 1;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return LINK_SOCK_INVALID;
    return (link_sock_t)s;
}

static int ws_bind_port(link_sock_t sock, unsigned port) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    return bind((SOCKET)sock, (struct sockaddr *)&sa, sizeof(sa)) == 0 ? 0 : -1;
}

static int ws_set_reuseaddr(link_sock_t sock) {
    BOOL one = TRUE;
    return setsockopt((SOCKET)sock, SOL_SOCKET, SO_REUSEADDR,
                      (const char *)&one, sizeof(one)) == 0 ? 0 : -1;
}

static int ws_set_nonblock(link_sock_t sock) {
    u_long one = 1;
    return ioctlsocket((SOCKET)sock, FIONBIO, &one) == 0 ? 0 : -1;
}

static int ws_send_to(link_sock_t sock, const void *buf, unsigned len,
                      const void *addr) {
    int r = sendto((SOCKET)sock, (const char *)buf, (int)len, 0,
                   (const struct sockaddr *)addr, LINK_ADDRLEN);
    return r < 0 ? -1 : r;
}

static int ws_recv_from(link_sock_t sock, void *buf, unsigned len,
                        void *addr, unsigned *addrlen) {
    int alen = LINK_ADDRLEN;
    int r = recvfrom((SOCKET)sock, (char *)buf, (int)len, 0,
                     (struct sockaddr *)addr, &alen);
    if (r < 0) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) return -1;
        return -1;
    }
    *addrlen = (unsigned)alen;
    return r;
}

static int ws_wait_readable(link_sock_t sock, int timeout_ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET((SOCKET)sock, &rf);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(0, &rf, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
    return r;  /* 1 ready, 0 timeout, SOCKET_ERROR(-1) error */
}

static void ws_close_sock(link_sock_t sock) {
    closesocket((SOCKET)sock);
}

static void ws_make_addr(void *addr_out, const char *ip, unsigned port) {
    struct sockaddr_in *sa = (struct sockaddr_in *)addr_out;
    memset(sa, 0, LINK_ADDRLEN);
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &sa->sin_addr);
}

static int ws_same_addr(const void *a, const void *b) {
    const struct sockaddr_in *x = (const struct sockaddr_in *)a;
    const struct sockaddr_in *y = (const struct sockaddr_in *)b;
    return x->sin_addr.s_addr == y->sin_addr.s_addr &&
           x->sin_port == y->sin_port;
}

static const struct link_sock_ops ws_ops = {
    ws_create_udp, ws_bind_port, ws_set_reuseaddr, ws_set_nonblock,
    ws_send_to, ws_recv_from, ws_wait_readable, ws_close_sock,
    ws_make_addr, ws_same_addr,
};

#elif LINK_POSIX

static link_sock_t px_create_udp(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    return s < 0 ? LINK_SOCK_INVALID : (link_sock_t)s;
}

static int px_bind_port(link_sock_t sock, unsigned port) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    return bind((int)sock, (struct sockaddr *)&sa, sizeof(sa)) == 0 ? 0 : -1;
}

static int px_set_reuseaddr(link_sock_t sock) {
    int one = 1;
    return setsockopt((int)sock, SOL_SOCKET, SO_REUSEADDR,
                     &one, sizeof(one)) == 0 ? 0 : -1;
}

static int px_set_nonblock(link_sock_t sock) {
    int fl = fcntl((int)sock, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl((int)sock, F_SETFL, fl | O_NONBLOCK) == 0 ? 0 : -1;
}

static int px_send_to(link_sock_t sock, const void *buf, unsigned len,
                      const void *addr) {
    ssize_t r = sendto((int)sock, buf, len, 0,
                       (const struct sockaddr *)addr, LINK_ADDRLEN);
    return r < 0 ? -1 : (int)r;
}

static int px_recv_from(link_sock_t sock, void *buf, unsigned len,
                        void *addr, unsigned *addrlen) {
    socklen_t alen = LINK_ADDRLEN;
    ssize_t r = recvfrom((int)sock, buf, len, 0,
                         (struct sockaddr *)addr, &alen);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        return -1;
    }
    *addrlen = (unsigned)alen;
    return (int)r;
}

static int px_wait_readable(link_sock_t sock, int timeout_ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET((int)sock, &rf);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select((int)sock + 1, &rf, NULL, NULL,
                   timeout_ms < 0 ? NULL : &tv);
    return r;  /* 1 ready, 0 timeout, -1 error */
}

static void px_close_sock(link_sock_t sock) {
    close((int)sock);
}

static void px_make_addr(void *addr_out, const char *ip, unsigned port) {
    struct sockaddr_in *sa = (struct sockaddr_in *)addr_out;
    memset(sa, 0, LINK_ADDRLEN);
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &sa->sin_addr);
}

static int px_same_addr(const void *a, const void *b) {
    const struct sockaddr_in *x = (const struct sockaddr_in *)a;
    const struct sockaddr_in *y = (const struct sockaddr_in *)b;
    return x->sin_addr.s_addr == y->sin_addr.s_addr &&
           x->sin_port == y->sin_port;
}

static const struct link_sock_ops px_ops = {
    px_create_udp, px_bind_port, px_set_reuseaddr, px_set_nonblock,
    px_send_to, px_recv_from, px_wait_readable, px_close_sock,
    px_make_addr, px_same_addr,
};

#endif /* backend */

static const struct link_sock_ops *g_ops_override;

const struct link_sock_ops *link_default_ops(void) {
#if defined(LINK_PS5_BACKEND)
    return 0;  /* PS5: install via link_ps5_install() before any link_* call */
#elif LINK_WINSOCK
    return &ws_ops;
#else
    return &px_ops;
#endif
}

void link_set_ops(const struct link_sock_ops *ops) {
    g_ops_override = ops;
}

static const struct link_sock_ops *ops_of(const struct link_endpoint *ep) {
    return ep->ops ? ep->ops : (g_ops_override ? g_ops_override
                                              : link_default_ops());
}

/* ------------------------------------------------------------- core ----- */

void link_init(struct link_endpoint *ep) {
    memset(ep, 0, sizeof(*ep));
    ep->sock = LINK_SOCK_INVALID;
    ep->role = LINK_ROLE_NONE;
}

int link_host(struct link_endpoint *ep, unsigned port) {
    const struct link_sock_ops *ops = ops_of(ep);
    link_disconnect(ep);
    link_sock_t s = ops->create_udp();
    if (s == LINK_SOCK_INVALID) return 0;
    ops->set_reuseaddr(s);
    if (ops->bind_port(s, port) != 0) { ops->close_sock(s); return 0; }
    ops->set_nonblock(s);
    ep->ops = ops;
    ep->sock = s;
    ep->role = LINK_ROLE_HOST;
    ep->linked = 0;
    ep->next_seq = 1;
    ep->port = port;
    return 1;
}

int link_join(struct link_endpoint *ep, const char *host_ip, unsigned port) {
    const struct link_sock_ops *ops = ops_of(ep);
    link_disconnect(ep);
    link_sock_t s = ops->create_udp();
    if (s == LINK_SOCK_INVALID) return 0;
    if (ops->bind_port(s, 0) != 0) { ops->close_sock(s); return 0; }
    ops->set_nonblock(s);
    ep->ops = ops;
    ep->sock = s;
    ep->role = LINK_ROLE_JOINER;
    ep->linked = 0;
    ep->next_seq = 1;
    ep->port = port;
    ops->make_addr(ep->peer_addr, host_ip, port);

    /* HELLO with retransmit until ACK or timeout. */
    uint8_t out[LINK_PKT_LEN], in[LINK_PKT_LEN];
    uint8_t from[LINK_ADDRLEN];
    int elapsed = 0;
    const int slice = 100;
    while (elapsed < LINK_JOIN_TIMEOUT_MS) {
        pkt_build(out, LINK_PKT_HELLO, 0, 0, 0);
        ops->send_to(s, out, LINK_PKT_LEN, ep->peer_addr);
        int w = ops->wait_readable(s, slice);
        if (w > 0) {
            for (;;) {
                unsigned alen = LINK_ADDRLEN;
                int r = ops->recv_from(s, in, sizeof(in), from, &alen);
                if (r < 0) break;
                uint8_t type, mode; uint32_t seq, data;
                if (!pkt_parse(in, (unsigned)r, &type, &mode, &seq, &data))
                    continue;
                if (type == LINK_PKT_HELLO_ACK &&
                    ops->same_addr(from, ep->peer_addr)) {
                    ep->linked = 1;
                    return 1;
                }
            }
        }
        elapsed += slice;
    }
    return 0;
}

void link_disconnect(struct link_endpoint *ep) {
    const struct link_sock_ops *ops = ep->ops ? ep->ops
        : (g_ops_override ? g_ops_override : link_default_ops());
    if (ep->sock != LINK_SOCK_INVALID) {
        if (ops) ops->close_sock(ep->sock);
        ep->sock = LINK_SOCK_INVALID;
    }
    ep->role = LINK_ROLE_NONE;
    ep->linked = 0;
}

int link_is_up(const struct link_endpoint *ep) {
    return ep->linked && ep->sock != LINK_SOCK_INVALID;
}

/* Drain one readable datagram; returns 1 if a valid packet was parsed. */
static int recv_pkt(struct link_endpoint *ep, uint8_t *type, uint8_t *mode,
                    uint32_t *seq, uint32_t *data, void *from) {
    const struct link_sock_ops *ops = ops_of(ep);
    uint8_t buf[LINK_PKT_LEN];
    unsigned alen = LINK_ADDRLEN;
    int r = ops->recv_from(ep->sock, buf, sizeof(buf), from, &alen);
    if (r < 0) return 0;
    return pkt_parse(buf, (unsigned)r, type, mode, seq, data);
}

int link_transfer_host(struct link_endpoint *ep, int mode, uint32_t out_data,
                       uint32_t *in_data, int timeout_ms) {
    const struct link_sock_ops *ops = ops_of(ep);
    uint32_t seq = ep->next_seq++;
    if (ep->next_seq == 0) ep->next_seq = 1;
    uint8_t out[LINK_PKT_LEN];
    pkt_build(out, LINK_PKT_REQ, (uint8_t)mode, seq, out_data);
    ops->send_to(ep->sock, out, LINK_PKT_LEN, ep->peer_addr);

    *in_data = 0xFFFFu;
    int elapsed = 0;
    int last_send = 0;
    const int slice = 25;
    uint8_t from[LINK_ADDRLEN];
    while (elapsed < timeout_ms) {
        int w = ops->wait_readable(ep->sock, slice);
        if (w < 0) break;
        if (w > 0) {
            for (;;) {
                uint8_t type, pmode; uint32_t pseq, pdata;
                if (!recv_pkt(ep, &type, &pmode, &pseq, &pdata, from)) break;
                if (!ops->same_addr(from, ep->peer_addr)) continue;
                if (type == LINK_PKT_RESP && pseq == seq) {
                    *in_data = pdata;
                    return 1;
                }
                /* Stale/duplicate packets are ignored. */
            }
        }
        elapsed += slice;
        /* The REQ or its RESP may have been lost: retransmit with the
         * SAME sequence number. The peer's dup guard makes this safe. */
        if (elapsed - last_send >= LINK_REQ_RESEND_MS &&
            elapsed < timeout_ms) {
            ops->send_to(ep->sock, out, LINK_PKT_LEN, ep->peer_addr);
            last_send = elapsed;
        }
    }
    return 1;  /* timeout: in_data already 0xFFFF (link error, not a hang) */
}

int link_transfer_sym(struct link_endpoint *ep, int mode, uint32_t out_data,
                      uint32_t *in_data, int timeout_ms) {
    const struct link_sock_ops *ops = ops_of(ep);
    uint32_t seq = ep->next_seq++;
    if (ep->next_seq == 0) ep->next_seq = 1;
    uint8_t out[LINK_PKT_LEN], resp[LINK_PKT_LEN];
    pkt_build(out, LINK_PKT_REQ, (uint8_t)mode, seq, out_data);
    ops->send_to(ep->sock, out, LINK_PKT_LEN, ep->peer_addr);

    *in_data = 0xFFFFu;
    int elapsed = 0;
    int last_send = 0;
    const int slice = 25;
    uint8_t from[LINK_ADDRLEN];
    while (elapsed < timeout_ms) {
        int w = ops->wait_readable(ep->sock, slice);
        if (w < 0) break;
        if (w > 0) {
            for (;;) {
                uint8_t type, pmode; uint32_t pseq, pdata;
                if (!recv_pkt(ep, &type, &pmode, &pseq, &pdata, from)) break;
                if (!ops->same_addr(from, ep->peer_addr)) continue;
                if (type == LINK_PKT_RESP && pseq == seq) {
                    *in_data = pdata;
                    return 1;
                }
                if (type == LINK_PKT_REQ) {
                    /* Peer is transferring too: answer immediately with
                     * our (unchanged) outgoing data, keep waiting. */
                    pkt_build(resp, LINK_PKT_RESP, pmode, pseq, out_data);
                    ops->send_to(ep->sock, resp, LINK_PKT_LEN, ep->peer_addr);
                }
            }
        }
        elapsed += slice;
        if (elapsed - last_send >= LINK_REQ_RESEND_MS &&
            elapsed < timeout_ms) {
            ops->send_to(ep->sock, out, LINK_PKT_LEN, ep->peer_addr);
            last_send = elapsed;
        }
    }
    return 1;
}

void link_poll(struct link_endpoint *ep, link_req_cb cb, void *ctx) {
    if (ep->sock == LINK_SOCK_INVALID) return;
    const struct link_sock_ops *ops = ops_of(ep);
    uint8_t from[LINK_ADDRLEN];
    /* Bound the work per call: the frame loop must stay real-time. */
    for (int n = 0; n < 32; n++) {
        uint8_t type, mode; uint32_t seq, data;
        if (!recv_pkt(ep, &type, &mode, &seq, &data, from)) break;
        if (type == LINK_PKT_HELLO && ep->role == LINK_ROLE_HOST) {
            if (!ep->linked) {
                memcpy(ep->peer_addr, from, LINK_ADDRLEN);
                ep->linked = 1;
            }
            /* Resend ACK for duplicate HELLOs from our peer: their ACK
             * was lost, so they're still retransmitting. HELLOs from any
             * other source while linked are ignored (one peer max). */
            if (ops->same_addr(from, ep->peer_addr)) {
                uint8_t ack[LINK_PKT_LEN];
                pkt_build(ack, LINK_PKT_HELLO_ACK, 0, 0, 0);
                ops->send_to(ep->sock, ack, LINK_PKT_LEN, ep->peer_addr);
            }
        } else if (type == LINK_PKT_HELLO_ACK && ep->role == LINK_ROLE_JOINER) {
            if (ops->same_addr(from, ep->peer_addr)) ep->linked = 1;
        } else if (type == LINK_PKT_REQ && ep->role == LINK_ROLE_JOINER &&
                   ep->linked && ops->same_addr(from, ep->peer_addr) && cb) {
            uint32_t answer = cb(ctx, mode, data, seq);
            if (answer != LINK_RESP_DEFERRED) {
                uint8_t resp[LINK_PKT_LEN];
                pkt_build(resp, LINK_PKT_RESP, mode, seq, answer);
                ops->send_to(ep->sock, resp, LINK_PKT_LEN, ep->peer_addr);
            }
        }
        /* Anything else (stray RESP, second HELLO) is ignored. */
    }
}

void link_send_resp(struct link_endpoint *ep, uint32_t seq, int mode,
                    uint32_t data) {
    const struct link_sock_ops *ops = ops_of(ep);
    if (!ep->linked || ep->role != LINK_ROLE_JOINER) return;
    uint8_t resp[LINK_PKT_LEN];
    pkt_build(resp, LINK_PKT_RESP, (uint8_t)mode, seq, data);
    ops->send_to(ep->sock, resp, LINK_PKT_LEN, ep->peer_addr);
}
