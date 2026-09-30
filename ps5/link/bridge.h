/* GbaC0re link-cable bridge: GBASIODriver glue.
 *
 * Attaches the UDP link (link_proto) to mGBA's SIO emulation. One bridge
 * per process; host is SIO player 0 (bus master), joiner is player 1.
 *
 * Multiplayer mode is asymmetric, like real hardware: only the master's
 * core calls start(), which does a blocking rendezvous. The joiner's
 * transfer is synthesized when the request arrives -- either immediately
 * in bridge_poll() (game already waiting on Busy) or on the Busy rising
 * edge seen by writeSIOCNT (game starts its wait after the packet lands).
 * Normal modes are symmetric: both sides rendezvous inside start().
 */
#ifndef BRIDGE_H
#define BRIDGE_H

#include "link_proto.h"

struct GBA;  /* mgba/internal/gba/gba.h */

#ifdef __cplusplus
extern "C" {
#endif

struct bridge *bridge_get(void);  /* singleton */

/* PS5 only: install the gadget-based socket backend. Must be called once
 * before bridge_host()/bridge_join(). PC/POSIX use the built-in backend. */
void bridge_ps5_set_ops(const struct link_sock_ops *ops);

/* Start hosting (bind port) or join a host. Returns 1 on success.
 * For join, blocks up to LINK_JOIN_TIMEOUT_MS hunting for the host. */
int bridge_host(unsigned port);
int bridge_join(const char *host_ip, unsigned port);
void bridge_disconnect(void);
int bridge_is_up(void);
int bridge_role(void);  /* LINK_ROLE_* */

/* Short status line for menus ("HOST: waiting", "LINKED (HOST)", ...). */
const char *bridge_status_text(void);

/* Frame pump: must be called once per emulation frame AND from menu loops
 * while link is active (keeps the joiner answering while paused). */
void bridge_poll(void);

/* Attach/detach the SIO driver to a loaded core. Safe to call with no ROM
 * (no-op). Detaching mid-session returns the game to "no cable". */
int bridge_attach(struct GBA *gba);
void bridge_detach(void);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_H */
