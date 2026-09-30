/* GbaC0re link-cable bridge: portable UDP protocol layer.
 *
 * No mGBA dependency here -- this is the wire protocol plus socket plumbing,
 * unit-testable on its own. The GBASIODriver glue lives in bridge.c.
 *
 * Roles: one side HOSTs (SIO player 0 / bus master), the other JOINs
 * (player 1 / slave). Multiplayer-mode transfers are master-initiated:
 * the host's start() does a blocking rendezvous per transfer; the joiner
 * answers from its frame loop via link_poll(). Normal-mode transfers are
 * symmetric: both sides rendezvous inside link_transfer_sym().
 *
 * No threads anywhere. The host's rendezvous blocks the emulation thread
 * (typically 1-2 frames); the joiner never blocks.
 */
#ifndef LINK_PROTO_H
#define LINK_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_DEFAULT_PORT 43879
#define LINK_MAGIC0 'G'
#define LINK_MAGIC1 'B'
#define LINK_MAGIC2 'L'
#define LINK_MAGIC3 '1'

/* Packet types. */
#define LINK_PKT_HELLO     1  /* joiner -> host: "I want to link" */
#define LINK_PKT_HELLO_ACK 2  /* host -> joiner: accepted */
#define LINK_PKT_REQ       3  /* host -> joiner: transfer request */
#define LINK_PKT_RESP      4  /* joiner -> host: transfer response */

/* Transfer modes (payload byte). */
#define LINK_MODE_MULTI    0  /* multiplayer 16-bit */
#define LINK_MODE_NORMAL32 1  /* normal 32-bit */
#define LINK_MODE_NORMAL8  2  /* normal 8-bit */

#define LINK_ROLE_NONE   0
#define LINK_ROLE_HOST   1
#define LINK_ROLE_JOINER 2

/* How long the host waits for one transfer's response before the game
 * sees a link error (0xFFFF). */
#define LINK_TRANSFER_TIMEOUT_MS 3000
/* REQ retransmit interval inside link_transfer_*: the same sequence number
 * is reused, so duplicates are idempotent on the peer. */
#define LINK_REQ_RESEND_MS 100
/* How long link_join() hunts for a host. */
#define LINK_JOIN_TIMEOUT_MS 5000

#define LINK_ADDRLEN 16  /* sockaddr_in-sized */

/* Opaque socket handle: int fd on POSIX/PS5, SOCKET on Windows. */
typedef intptr_t link_sock_t;
#define LINK_SOCK_INVALID ((link_sock_t)-1)

/* Socket backend. POSIX and Winsock are built in; the PS5 payload supplies
 * its own table (gadget calls) via link_set_ops(). */
struct link_sock_ops {
    link_sock_t (*create_udp)(void);
    int (*bind_port)(link_sock_t sock, unsigned port); /* 0 ok */
    int (*set_reuseaddr)(link_sock_t sock);            /* 0 ok */
    int (*set_nonblock)(link_sock_t sock);             /* 0 ok */
    /* Returns bytes sent (<0 on error). addr is LINK_ADDRLEN bytes. */
    int (*send_to)(link_sock_t sock, const void *buf, unsigned len,
                   const void *addr);
    /* Returns bytes received; -1 means "nothing waiting" (not an error).
     * addr/addrlen are in/out; addrlen must start at LINK_ADDRLEN. */
    int (*recv_from)(link_sock_t sock, void *buf, unsigned len,
                     void *addr, unsigned *addrlen);
    /* 1 = readable, 0 = timeout, <0 = error. */
    int (*wait_readable)(link_sock_t sock, int timeout_ms);
    void (*close_sock)(link_sock_t sock);
    /* Parse "192.168.0.140" + port into LINK_ADDRLEN-byte sockaddr. */
    void (*make_addr)(void *addr_out, const char *ip, unsigned port);
    int (*same_addr)(const void *a, const void *b);
};

struct link_endpoint {
    const struct link_sock_ops *ops;
    link_sock_t sock;
    int role;
    int linked;
    uint8_t peer_addr[LINK_ADDRLEN];
    uint32_t next_seq;
    unsigned port;
};

/* Built-in backend (POSIX, or Winsock on _WIN32). */
const struct link_sock_ops *link_default_ops(void);
/* Override (PS5). Must be called before any other link_* function. */
void link_set_ops(const struct link_sock_ops *ops);

#ifdef LINK_PS5
/* PS5 backend install. The payload resolves the libkernel symbols at boot,
 * fills this struct, and calls link_ps5_install() once. Field order and
 * types are ABI between the payload's main.c and link_ps5.c. */
typedef unsigned long long link_ps5_u64;
typedef link_ps5_u64 (*link_ps5_nc_t)(void *gadget, void *fn,
    link_ps5_u64 a0, link_ps5_u64 a1, link_ps5_u64 a2,
    link_ps5_u64 a3, link_ps5_u64 a4, link_ps5_u64 a5);
struct link_ps5_fns {
    link_ps5_nc_t nc;
    void *gadget;
    void *socket_fn;     /* int socket(int, int, int) */
    void *bind_fn;       /* int bind(int, const void *, int) */
    void *recvfrom_fn;
    void *sendto_fn;
    void *poll_fn;
    void *setsockopt_fn; /* optional; NULL = skip */
    void *close_fn;      /* sceKernelClose */
};
void link_ps5_install(const struct link_ps5_fns *fns);
#endif

void link_init(struct link_endpoint *ep);

/* Host: bind port, non-blocking. Returns 1 on success. The handshake
 * completes later inside link_poll() when a HELLO arrives. */
int link_host(struct link_endpoint *ep, unsigned port);
/* Join: bind ephemeral, send HELLO, retransmit until ACK or timeout.
 * Returns 1 if the host answered. Blocks up to LINK_JOIN_TIMEOUT_MS. */
int link_join(struct link_endpoint *ep, const char *host_ip, unsigned port);
void link_disconnect(struct link_endpoint *ep);
int link_is_up(const struct link_endpoint *ep);

/* Host-side multiplayer rendezvous: send REQ(mode, out_data), wait for the
 * RESP with the same sequence number. Returns 1; in_data is 0xFFFF on
 * timeout (game sees a link error, not a hang). */
int link_transfer_host(struct link_endpoint *ep, int mode, uint32_t out_data,
                       uint32_t *in_data, int timeout_ms);

/* Symmetric rendezvous for normal mode (both sides call this): send
 * REQ, answer any incoming REQs while waiting, return on matching RESP.
 * Returns 1; in_data is 0xFFFF... on timeout (same error convention). */
int link_transfer_sym(struct link_endpoint *ep, int mode, uint32_t out_data,
                      uint32_t *in_data, int timeout_ms);

/* Joiner-side pump: call once per frame. Handles HELLO_ACK and incoming
 * REQs. For each REQ, cb() supplies the response word and may complete the
 * local transfer immediately.
 *
 * Deferred responses: if the app can't answer yet (e.g. the local game
 * hasn't armed its transfer), cb() returns LINK_RESP_DEFERRED and link_poll
 * skips the RESP. The app must later call link_send_resp() with the same
 * seq. A retransmitted REQ for a deferred transfer invokes cb() again. */
#define LINK_RESP_DEFERRED 0xFFFFFFFFu
typedef uint32_t (*link_req_cb)(void *ctx, int mode, uint32_t out_data,
                                uint32_t seq);
void link_poll(struct link_endpoint *ep, link_req_cb cb, void *ctx);

/* Send a RESP for a previously deferred REQ. Only valid while linked and
 * for the peer address the REQ came from. */
void link_send_resp(struct link_endpoint *ep, uint32_t seq, int mode,
                    uint32_t data);

#ifdef __cplusplus
}
#endif

#endif /* LINK_PROTO_H */
