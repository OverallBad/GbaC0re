/* GbaC0re link-cable bridge: PS5 socket backend.
 *
 * Freestanding: no system headers, no libc socket API. Every call goes
 * through the native_call gadget with libkernel symbols resolved by the
 * payload at boot and installed here via link_ps5_install().
 *
 * The socket stays in blocking mode; receives are always guarded by a
 * poll() with timeout 0 first, so recv_from() never blocks the emulation
 * thread. This avoids needing FIONBIO/fcntl constants on the PS5.
 */
#include "link_proto.h"  /* compile with -DLINK_PS5 */

typedef link_ps5_u64 ps5_u64;
typedef long long ps5_s64;
typedef link_ps5_nc_t ps5_nc_t;

static struct link_ps5_fns F;
static int F_ok;

static ps5_u64 nc6(void *fn, ps5_u64 a0, ps5_u64 a1, ps5_u64 a2,
                   ps5_u64 a3, ps5_u64 a4, ps5_u64 a5) {
    return F.nc(F.gadget, fn, a0, a1, a2, a3, a4, a5);
}

/* sockaddr_in, 16 bytes: family LE u16, port BE u16, addr BE u32, zero. */
static void sa_build(unsigned char *o, unsigned port,
                     unsigned char a, unsigned char b,
                     unsigned char c, unsigned char d) {
    for (int i = 0; i < 16; i++) o[i] = 0;
    o[0] = 2; o[1] = 0;  /* AF_INET */
    o[2] = (unsigned char)(port >> 8);
    o[3] = (unsigned char)(port & 0xFF);
    o[4] = a; o[5] = b; o[6] = c; o[7] = d;
}

static link_sock_t ps5_create_udp(void) {
    if (!F_ok) return LINK_SOCK_INVALID;
    /* AF_INET=2, SOCK_DGRAM=2 */
    ps5_s64 fd = (ps5_s64)nc6(F.socket_fn, 2, 2, 0, 0, 0, 0);
    return fd < 0 ? LINK_SOCK_INVALID : (link_sock_t)fd;
}

static int ps5_bind_port(link_sock_t sock, unsigned port) {
    unsigned char sa[16];
    sa_build(sa, port, 0, 0, 0, 0);
    ps5_s64 r = (ps5_s64)nc6(F.bind_fn, (ps5_u64)sock, (ps5_u64)sa, 16,
                             0, 0, 0);
    return r == 0 ? 0 : -1;
}

static int ps5_set_reuseaddr(link_sock_t sock) {
    if (!F.setsockopt_fn) return 0;
    /* SOL_SOCKET=0xffff, SO_REUSEADDR=0x0004 (FreeBSD-derived). Best
     * effort: UDP has no TIME_WAIT, so failure is harmless. */
    int one = 1;
    nc6(F.setsockopt_fn, (ps5_u64)sock, 0xFFFFu, 4, (ps5_u64)&one, 4, 0);
    return 0;
}

static int ps5_set_nonblock(link_sock_t sock) {
    (void)sock;
    /* No-op: receives are poll-guarded, never blocking. */
    return 0;
}

static int ps5_send_to(link_sock_t sock, const void *buf, unsigned len,
                       const void *addr) {
    ps5_s64 r = (ps5_s64)nc6(F.sendto_fn, (ps5_u64)sock, (ps5_u64)buf,
                             len, 0, (ps5_u64)addr, 16);
    return (int)r;
}

static int ps5_poll_fd(link_sock_t sock, int timeout_ms) {
    unsigned char pfd[8];
    *(int *)pfd = (int)sock;
    *(unsigned short *)(pfd + 4) = 1;  /* POLLIN */
    *(unsigned short *)(pfd + 6) = 0;
    ps5_s64 r = (ps5_s64)nc6(F.poll_fn, (ps5_u64)pfd, 1,
                             (ps5_u64)(ps5_s64)timeout_ms, 0, 0, 0);
    return r > 0 ? 1 : (r == 0 ? 0 : -1);
}

static int ps5_recv_from(link_sock_t sock, void *buf, unsigned len,
                         void *addr, unsigned *addrlen) {
    if (ps5_poll_fd(sock, 0) != 1) return -1;  /* nothing waiting */
    int alen = 16;
    ps5_s64 r = (ps5_s64)nc6(F.recvfrom_fn, (ps5_u64)sock, (ps5_u64)buf,
                             len, 0, (ps5_u64)addr, (ps5_u64)&alen);
    if (r < 0) return -1;
    if (addrlen) *addrlen = (unsigned)alen;
    return (int)r;
}

static int ps5_wait_readable(link_sock_t sock, int timeout_ms) {
    return ps5_poll_fd(sock, timeout_ms);
}

static void ps5_close_sock(link_sock_t sock) {
    nc6(F.close_fn, (ps5_u64)sock, 0, 0, 0, 0, 0);
}

static void ps5_make_addr(void *addr_out, const char *ip, unsigned port) {
    unsigned char *o = (unsigned char *)addr_out;
    unsigned char q[4] = { 0, 0, 0, 0 };
    unsigned v = 0, qi = 0;
    for (const char *p = ip; ; p++) {
        if (*p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned)(*p - '0');
        } else {
            if (qi < 4) q[qi++] = (unsigned char)v;
            v = 0;
            if (*p == 0 || qi == 4) break;
        }
    }
    sa_build(o, port, q[0], q[1], q[2], q[3]);
}

static int ps5_same_addr(const void *a, const void *b) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (int i = 0; i < 16; i++) {
        if (x[i] != y[i]) return 0;
    }
    return 1;
}

static const struct link_sock_ops ps5_ops = {
    ps5_create_udp,
    ps5_bind_port,
    ps5_set_reuseaddr,
    ps5_set_nonblock,
    ps5_send_to,
    ps5_recv_from,
    ps5_wait_readable,
    ps5_close_sock,
    ps5_make_addr,
    ps5_same_addr,
};

/* Called once by the payload at boot, after dlsym resolution. Installs the
 * backend globally so bridge_host()/bridge_join() just work. */
void link_ps5_install(const struct link_ps5_fns *fns) {
    F = *fns;
    F_ok = (fns->nc && fns->gadget && fns->socket_fn && fns->bind_fn &&
            fns->recvfrom_fn && fns->sendto_fn && fns->poll_fn &&
            fns->close_fn) ? 1 : 0;
    if (F_ok) link_set_ops(&ps5_ops);
}
