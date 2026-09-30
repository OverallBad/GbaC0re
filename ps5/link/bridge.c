/* GbaC0re link-cable bridge: GBASIODriver glue. See bridge.h. */
#include "bridge.h"

#include <string.h>
#include <stdio.h>

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/sio.h>
#include <mgba/gba/interface.h>

#define BRIDGE_DRIVER_ID 0x47424C4E  /* 'GBLN' */

struct bridge {
    struct GBASIODriver base;  /* must be first */
    struct link_endpoint link;
    struct GBA *gba;           /* core we're attached to (or NULL) */
    int mode;                  /* last setMode value */
    uint32_t peer_data;        /* last transfer's received word */
    uint16_t own_multi;        /* own word sent in last multiplayer xfer */
    /* Joiner-side pending master transfer (packet arrived before the
     * game set Busy). */
    int slave_pending;
    uint16_t slave_master_data;
    uint32_t slave_pending_seq; /* seq of the stashed (not yet consumed) REQ */
    uint32_t slave_done_seq;   /* last completed REQ seq (dup guard) */
    uint16_t slave_last_answer; /* RESP data sent for slave_done_seq */
    char status[64];
};

static struct bridge g_bridge;
static int g_inited;

static struct bridge *bself(struct GBASIODriver *d) {
    return (struct bridge *)d;
}

/* ------------------------------------------------- driver callbacks --- */

static bool bridge_init(struct GBASIODriver *driver) {
    struct bridge *b = bself(driver);
    if (!g_inited) {
        link_init(&b->link);
        g_inited = 1;
    }
    return true;
}

static void bridge_deinit(struct GBASIODriver *driver) {
    struct bridge *b = bself(driver);
    /* Don't tear down the UDP session here: the SIO core is deinitialized
     * on ROM change while the link session should survive it. Socket
     * lifetime is owned by bridge_host()/bridge_join()/bridge_disconnect(). */
    b->gba = NULL;
}

static void bridge_reset(struct GBASIODriver *driver) {
    struct bridge *b = bself(driver);
    b->peer_data = 0;
    b->own_multi = 0;
    b->slave_pending = 0;
    b->slave_pending_seq = 0;
    b->slave_done_seq = 0;
    b->slave_last_answer = 0;
}

static uint32_t bridge_driver_id(const struct GBASIODriver *driver) {
    (void)driver;
    return BRIDGE_DRIVER_ID;
}

static void bridge_set_mode(struct GBASIODriver *driver,
                            enum GBASIOMode mode) {
    bself(driver)->mode = (int)mode;
}

static bool bridge_handles_mode(struct GBASIODriver *driver,
                                enum GBASIOMode mode) {
    (void)driver;
    return mode == GBA_SIO_MULTI || mode == GBA_SIO_NORMAL_8 ||
           mode == GBA_SIO_NORMAL_32;
}

static int bridge_connected_devices(struct GBASIODriver *driver) {
    struct bridge *b = bself(driver);
    /* mGBA semantics (cf. lockstep's nAttached - 1): number of OTHER
     * devices, not the total. Drives the Slave bit and transfer timing. */
    return link_is_up(&b->link) ? 1 : 0;
}

static int bridge_device_id(struct GBASIODriver *driver) {
    struct bridge *b = bself(driver);
    return b->link.role == LINK_ROLE_JOINER ? 1 : 0;
}

/* Write the joiner-side completion into the core: data registers, Id,
 * IRQ. The caller clears the Busy bit (directly, or in the SIOCNT value
 * being written, depending on which path invoked us). */
static void slave_complete_regs(struct bridge *b, uint16_t master_data) {
    struct GBASIO *sio = b->base.p;
    struct GBA *gba = sio->p;
    /* The send register is SIOMLT_SEND (0x12A), not SIOMULTI0: the core
     * clears SIOMULTI0-3 to 0xFFFF on the master's Busy edge, and SIOMULTI0
     * is player 0's RECEIVE slot. mGBA's lockstep reads SIOMLT_SEND. */
    uint16_t own = gba->memory.io[GBA_REG_SIOMLT_SEND];
    gba->memory.io[GBA_REG_SIOMULTI0] = master_data;  /* player 0's word */
    gba->memory.io[GBA_REG_SIOMULTI1] = own;          /* player 1's word */
    gba->memory.io[GBA_REG_SIOMULTI2] = 0xFFFF;
    gba->memory.io[GBA_REG_SIOMULTI3] = 0xFFFF;
    sio->siocnt = GBASIOMultiplayerSetId(sio->siocnt, 1);
    if (GBASIOMultiplayerIsIrq(sio->siocnt))
        GBARaiseIRQ(gba, GBA_IRQ_SIO, 0);
}

static uint16_t bridge_write_siocnt(struct GBASIODriver *driver,
                                    uint16_t value) {
    struct bridge *b = bself(driver);
    struct GBASIO *sio = b->base.p;
    if (sio && b->mode == GBA_SIO_MULTI &&
        b->link.role == LINK_ROLE_JOINER) {
        /* Busy rising edge on the slave: if a master transfer is already
         * waiting, complete it now. NOTE: the core stores our return value
         * into sio->siocnt afterwards, so the Busy bit must be cleared in
         * the value we return, not just in sio->siocnt. */
        if (GBASIOMultiplayerIsBusy(value) &&
            !GBASIOMultiplayerIsBusy(sio->siocnt) && b->slave_pending) {
            /* The deferred REQ's answer: the game's SIOMLT_SEND word is
             * fresh now. Complete locally, then send the RESP we owed. */
            uint16_t own = sio->p->memory.io[GBA_REG_SIOMLT_SEND];
            slave_complete_regs(b, b->slave_master_data);
            value = GBASIOMultiplayerClearBusy(value);
            b->slave_pending = 0;
            link_send_resp(&b->link, b->slave_pending_seq,
                           LINK_MODE_MULTI, own);
            b->slave_done_seq = b->slave_pending_seq;
            b->slave_last_answer = own;
            b->own_multi = own;
            b->slave_pending_seq = 0;
        }
    }
    return value;
}

static bool bridge_start(struct GBASIODriver *driver) {
    struct bridge *b = bself(driver);
    struct GBASIO *sio = b->base.p;
    struct GBA *gba = sio ? sio->p : NULL;
    uint32_t in = 0xFFFFu;

    if (!gba || !link_is_up(&b->link)) {
        b->peer_data = 0xFFFFu;
        return true;
    }

    if (b->mode == GBA_SIO_MULTI) {
        if (b->link.role != LINK_ROLE_HOST) {
            /* Slave never starts multiplayer transfers (see bridge.h). */
            b->peer_data = 0xFFFFu;
            return true;
        }
        /* SIOMLT_SEND: the core already cleared SIOMULTI0-3 to 0xFFFF on
         * the Busy edge before calling us. */
        b->own_multi = gba->memory.io[GBA_REG_SIOMLT_SEND];
        link_transfer_host(&b->link, LINK_MODE_MULTI, b->own_multi, &in,
                           LINK_TRANSFER_TIMEOUT_MS);
        b->peer_data = in & 0xFFFFu;
        return true;
    }

    if (b->mode == GBA_SIO_NORMAL_32 || b->mode == GBA_SIO_NORMAL_8) {
        int lm = (b->mode == GBA_SIO_NORMAL_32) ? LINK_MODE_NORMAL32
                                                : LINK_MODE_NORMAL8;
        uint32_t out = (uint32_t)gba->memory.io[GBA_REG_SIODATA32_LO] |
                       ((uint32_t)gba->memory.io[GBA_REG_SIODATA32_HI] << 16);
        link_transfer_sym(&b->link, lm, out, &in, LINK_TRANSFER_TIMEOUT_MS);
        b->peer_data = in;
        return true;
    }

    return true;
}

static void bridge_finish_multiplayer(struct GBASIODriver *driver,
                                      uint16_t data[4]) {
    struct bridge *b = bself(driver);
    int id = bridge_device_id(driver);
    /* data[i] = player i's word, as seen by THIS station. */
    data[0] = b->own_multi;
    data[1] = (uint16_t)b->peer_data;
    data[2] = 0xFFFF;
    data[3] = 0xFFFF;
    if (id == 1) {
        /* Joiner is player 1: slot 0 is the master's word. */
        data[0] = (uint16_t)b->peer_data;
        data[1] = b->own_multi;
    }
}

static uint8_t bridge_finish_normal8(struct GBASIODriver *driver) {
    return (uint8_t)(bself(driver)->peer_data & 0xFFu);
}

static uint32_t bridge_finish_normal32(struct GBASIODriver *driver) {
    return bself(driver)->peer_data;
}

/* Joiner poll callback: answer a master REQ, completing the local side. */
static uint32_t on_link_req(void *ctx, int mode, uint32_t out_data,
                            uint32_t seq) {
    struct bridge *b = (struct bridge *)ctx;
    struct GBASIO *sio = b->base.p;
    if (!sio || !sio->p) return 0xFFFFu;

    if (mode == LINK_MODE_MULTI) {
        struct GBA *gba = sio->p;
        if (seq == b->slave_done_seq) {
            /* Duplicate of an already-answered transfer (our RESP was
             * lost and the host retransmitted): resend the cached
             * answer, don't synthesize twice. */
            return b->slave_last_answer;
        }
        if (GBASIOMultiplayerIsBusy(sio->siocnt)) {
            /* Game is already waiting: its SIOMLT_SEND word is fresh.
             * Complete right now. */
            uint16_t own = gba->memory.io[GBA_REG_SIOMLT_SEND];
            slave_complete_regs(b, (uint16_t)out_data);
            sio->siocnt = GBASIOMultiplayerClearBusy(sio->siocnt);
            b->slave_pending = 0;
            b->slave_done_seq = seq;
            b->slave_last_answer = own;
            b->own_multi = own;
            return own;
        }
        /* Packet beat the game: the game hasn't written SIOMLT_SEND yet,
         * so we can't answer. Stash and defer the RESP until the Busy
         * rising edge, when the fresh word is known. A retransmit of
         * this seq while deferred just re-stashes harmlessly. */
        b->slave_pending = 1;
        b->slave_master_data = (uint16_t)out_data;
        b->slave_pending_seq = seq;
        return LINK_RESP_DEFERRED;
    }

    /* Normal-mode REQs never reach the joiner poll (symmetric path), but
     * answer sanely if they do. */
    struct GBA *gba = sio->p;
    return (uint32_t)gba->memory.io[GBA_REG_SIODATA32_LO] |
           ((uint32_t)gba->memory.io[GBA_REG_SIODATA32_HI] << 16);
}

/* ------------------------------------------------------------ API ------- */

struct bridge *bridge_get(void) {
    return &g_bridge;
}

void bridge_ps5_set_ops(const struct link_sock_ops *ops) {
    link_set_ops(ops);
}

int bridge_host(unsigned port) {
    struct bridge *b = &g_bridge;
    if (!g_inited) { link_init(&b->link); g_inited = 1; }
    return link_host(&b->link, port ? port : LINK_DEFAULT_PORT);
}

int bridge_join(const char *host_ip, unsigned port) {
    struct bridge *b = &g_bridge;
    if (!g_inited) { link_init(&b->link); g_inited = 1; }
    int ok = link_join(&b->link, host_ip, port ? port : LINK_DEFAULT_PORT);
    if (!ok) link_disconnect(&b->link);  /* don't leak the socket/fd */
    return ok;
}

void bridge_disconnect(void) {
    struct bridge *b = &g_bridge;
    link_disconnect(&b->link);
    b->slave_pending = 0;
}

int bridge_is_up(void) {
    return link_is_up(&g_bridge.link);
}

int bridge_role(void) {
    return g_bridge.link.role;
}

const char *bridge_status_text(void) {
    struct bridge *b = &g_bridge;
    if (b->link.role == LINK_ROLE_NONE)
        return "LINK: OFF";
    const char *role = b->link.role == LINK_ROLE_HOST ? "HOST" : "JOIN";
    if (b->link.linked)
        snprintf(b->status, sizeof(b->status), "LINK: %s (CONNECTED)", role);
    else
        snprintf(b->status, sizeof(b->status), "LINK: %s (WAITING)", role);
    return b->status;
}

void bridge_poll(void) {
    struct bridge *b = &g_bridge;
    if (b->link.sock == LINK_SOCK_INVALID) return;
    link_poll(&b->link, on_link_req, b);
}

int bridge_attach(struct GBA *gba) {
    struct bridge *b = &g_bridge;
    if (!gba) return 0;
    if (!g_inited) { link_init(&b->link); g_inited = 1; }
    b->base.p = &gba->sio;
    b->base.init = bridge_init;
    b->base.deinit = bridge_deinit;
    b->base.reset = bridge_reset;
    b->base.driverId = bridge_driver_id;
    b->base.loadState = NULL;
    b->base.saveState = NULL;
    b->base.setMode = bridge_set_mode;
    b->base.handlesMode = bridge_handles_mode;
    b->base.connectedDevices = bridge_connected_devices;
    b->base.deviceId = bridge_device_id;
    b->base.writeSIOCNT = bridge_write_siocnt;
    b->base.writeRCNT = NULL;
    b->base.start = bridge_start;
    b->base.finishMultiplayer = bridge_finish_multiplayer;
    b->base.finishNormal8 = bridge_finish_normal8;
    b->base.finishNormal32 = bridge_finish_normal32;
    b->gba = gba;
    gba->sio.driver = &b->base;
    bridge_init(&b->base);
    return 1;
}

void bridge_detach(void) {
    struct bridge *b = &g_bridge;
    if (b->gba) {
        b->gba->sio.driver = NULL;
        b->gba = NULL;
    }
}
