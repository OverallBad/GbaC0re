#include "core.h"
#include "shim.h"
#include "gba_glue.h"
#include "savestate.h"
#include "ui.h"
#include "menu.h"
#include "savedata.h"
#include "bridge.h"  /* link-cable bridge (PC<->PS5 netplay) */
#include <stdio.h>
#include <string.h>

/* Arena backing the shim's malloc: a 32MB ROM image plus the mGBA core,
   save buffers and audio rings, with room to spare. */
/* Arena size is chosen at boot; see the ladder in _start. */

/* Frames R1 must be held before quitting actually happens. ~1s at 60Hz.
   See the play loop: an accidental quit costs a full Luac0re relaunch. */

static const char RESP_204K[] = "HTTP/1.1 204\r\nConnection:keep-alive\r\nAccess-Control-Allow-Origin:*\r\n\r\n";
static const char RESP_CORS[] = "HTTP/1.1 204\r\nAccess-Control-Allow-Origin:*\r\nAccess-Control-Allow-Methods:POST\r\nConnection:keep-alive\r\n\r\n";
static const char RESP_200[] = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin:*\r\nContent-Length:2\r\nConnection:close\r\n\r\nOK";
static const char RESP_400[] = "HTTP/1.1 400 Bad Request\r\nConnection:close\r\n\r\n";
static const char RESP_409[] = "HTTP/1.1 409 Conflict\r\nConnection:close\r\n\r\n";
static const char RESP_500[] = "HTTP/1.1 500 Internal Error\r\nConnection:close\r\n\r\n";

/* Menu is composed here at UI_W x UI_H (16:9) and goes out through blit_ui(),
   which is a separate path from the game's blit_scale() on purpose. In .bss,
   which linker.ld marks NOLOAD, so its size costs nothing in the blob. */
static u32 ui_screen[UI_W * UI_H];

/* Pause-menu backdrop at GBA resolution, dimmed in cached RAM. The game
   frame is frozen while the core is paused; v1.1.3 keeps the dimmed copy
   here (not in video memory) because reads from the write-combining video
   mapping were the v1.1.x menu lag. 240x160x4 = 150KB, also NOLOAD. */
static u32 menu_dim[GBA_W * GBA_H];

/* ------------------------------------------------------- data bootstrap -- */

#include "boot.inc"
#include "fault.inc"


/* ------------------------------------------------- OFW ROM upload ----
 * POST /rom?name=<file> streams a ROM into TEMP_ROM_DIR. OFW has no save
 * manager, so ROMs cannot be injected into /savedata0/roms/; the network is
 * the only door. One upload at a time; web_handle does bounded work per call
 * (a few 64KB recvs) so a 32MB transfer can't starve pad input or pacing. */

#define UPLOAD_MAX    (32u * 1024u * 1024)
#define UPLOAD_CHUNK  (64 * 1024)
#define UPLOAD_ITERS  4        /* recv iterations per web_handle call */
#define UPLOAD_STALL  1800     /* ~30s of no progress before aborting */

static int poll_ready(void *G, void *poll, s32 fd, s32 timeout_ms);

struct upload_state {
    s32  client_fd;            /* -1 when idle */
    s32  file_fd;
    u64  remaining;            /* body bytes still to write */
    int  idle_calls;           /* consecutive pump calls with no progress */
    char name[80];
    char path[128];
};

static struct upload_state up_state = { -1, -1, 0, 0, { 0 }, { 0 } };
static u8 *up_buf = 0;         /* 64KB scratch, mmap'd on first upload */

/* Batch ROM upload tracking. The launcher sends POST /roms_begin?count=N
 * before uploading N ROMs; the "no ROMs" wait loop shows progress and only
 * opens the picker once all N have arrived. */
static int roms_expected = 0;
static int roms_received = 0;

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode %XX in place. Returns the decoded length, or -1 on a
   malformed sequence. */
static int url_decode(char *s) {
    int r = 0, w = 0;
    while (s[r]) {
        if (s[r] == '%') {
            if (!s[r+1] || !s[r+2]) return -1;
            int hi = hexval(s[r+1]), lo = hexval(s[r+2]);
            if (hi < 0 || lo < 0) return -1;
            s[w++] = (char)(hi * 16 + lo);
            r += 3;
        } else {
            s[w++] = s[r++];
        }
    }
    s[w] = 0;
    return w;
}

/* Filename gate: [A-Za-z0-9._-], 1..64 chars, no leading dot, no "..",
   and it must name a .gba (case-insensitive, via is_rom_file). */
static int upload_name_ok(const char *name) {
    int len = str_len(name);
    if (len < 5 || len > 64) return 0;
    if (name[0] == '.') return 0;
    for (int i = 0; i < len; i++) {
        char c = name[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return 0;
        if (c == '.' && i + 1 < len && name[i+1] == '.') return 0;
    }
    return is_rom_file(name);
}

/* "content-length:" header value, matched case-insensitively. Returns 0 when
   absent or malformed. */
static u64 header_content_length(u8 *buf, s32 len) {
    static const char key[] = "content-length:";
    for (s32 i = 0; i + 15 <= len; i++) {
        int m = 1;
        for (int k = 0; k < 15; k++) {
            char a = (char)buf[i+k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (a != key[k]) { m = 0; break; }
        }
        if (!m) continue;
        s32 j = i + 15;
        while (j < len && (buf[j] == ' ' || buf[j] == '\t')) j++;
        u64 v = 0;
        int digits = 0;
        while (j < len && buf[j] >= '0' && buf[j] <= '9' && digits < 10) {
            v = v * 10 + (u64)(buf[j] - '0');
            j++; digits++;
        }
        return digits ? v : 0;
    }
    return 0;
}

/* Offset just past the header block's terminating \r\n\r\n, or -1. */
static s32 header_end(u8 *buf, s32 len) {
    for (s32 i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n')
            return i + 4;
    return -1;
}

/* Drop a failed upload's partial file. unlink() is preferred; when it didn't
   resolve, re-create with O_TRUNC so at worst a 0-byte stub remains. */
static void upload_discard(void *G, void *kopen, void *kclose, void *kunlink) {
    if (kunlink && up_state.path[0])
        NC(G, kunlink, (u64)up_state.path, 0,0,0,0,0);
    else if (kopen && up_state.path[0]) {
        s32 fd = (s32)NC(G, kopen, (u64)up_state.path,
                         0x601 /* O_WRONLY|O_CREAT|O_TRUNC */, 0x1FF, 0, 0, 0);
        if (fd >= 0) NC(G, kclose, (u64)fd, 0,0,0,0,0);
    }
}

static void upload_reset(void) {
    up_state.client_fd = -1;
    up_state.file_fd = -1;
    up_state.remaining = 0;
    up_state.idle_calls = 0;
    up_state.name[0] = 0;
    up_state.path[0] = 0;
}

static void upload_abort(void *G, void *kopen, void *kclose, void *kunlink,
                         const char *why) {
    if (up_state.file_fd >= 0)
        NC(G, kclose, (u64)up_state.file_fd, 0,0,0,0,0);
    if (up_state.client_fd >= 0)
        NC(G, kclose, (u64)up_state.client_fd, 0,0,0,0,0);
    upload_discard(G, kopen, kclose, kunlink);
    klog("upload: aborted (");
    klog(why);
    klog(")\n");
    upload_reset();
}

/* Start a POST /rom upload. The first body bytes may already sit in req.
   The client is always consumed (closed or owned); returns 1. */
static int upload_begin(void *G, void *send,
                        void *close, void *kopen, void *kwrite, void *kclose,
                        void *kmkdir, void *kunlink, void *mmap_fn,
                        s32 client, u8 *req, s32 n) {
    /* Request line is "POST /rom?name=<file> HTTP/1.1". */
    s32 qi = -1;
    for (s32 i = 9; i + 5 < n; i++)
        if (req[i] == 'n' && req[i+1] == 'a' && req[i+2] == 'm' &&
            req[i+3] == 'e' && req[i+4] == '=') { qi = i + 5; break; }

    char raw[128];
    int rl = 0;
    if (qi >= 0) {
        while (qi < n && rl < 127 && req[qi] != ' ' && req[qi] != '&' &&
               req[qi] != '\r' && req[qi] != '\n')
            raw[rl++] = (char)req[qi++];
    }
    raw[rl] = 0;

    s32 he = header_end(req, n);
    u64 clen = header_content_length(req, n);

    if (rl <= 0 || url_decode(raw) < 0 || !upload_name_ok(raw) ||
        he < 0 || clen < 1 || clen > UPLOAD_MAX) {
        NC(G, send, (u64)client, (u64)RESP_400, (u64)str_len(RESP_400), 0, 0, 0);
        NC(G, close, (u64)client, 0,0,0,0,0);
        return 1;
    }

    if (up_state.client_fd >= 0) {
        NC(G, send, (u64)client, (u64)RESP_409, (u64)str_len(RESP_409), 0, 0, 0);
        NC(G, close, (u64)client, 0,0,0,0,0);
        return 1;
    }

    if (kmkdir) NC(G, kmkdir, (u64)TEMP_ROM_DIR, 0x1FF, 0, 0, 0, 0);

    int p = 0;
    const char *d = TEMP_ROM_DIR;
    while (*d && p < 120) up_state.path[p++] = *d++;
    for (int i = 0; raw[i] && p < 127; i++) up_state.path[p++] = raw[i];
    up_state.path[p] = 0;
    for (int i = 0; i <= rl && i < 79; i++) up_state.name[i] = raw[i];

    s32 fd = (s32)NC(G, kopen, (u64)up_state.path,
                     0x601 /* O_WRONLY|O_CREAT|O_TRUNC */, 0x1FF, 0, 0, 0);
    if (fd < 0) {
        NC(G, send, (u64)client, (u64)RESP_500, (u64)str_len(RESP_500), 0, 0, 0);
        NC(G, close, (u64)client, 0,0,0,0,0);
        up_state.path[0] = 0;
        return 1;
    }

    if (!up_buf) {
        up_buf = (u8 *)NC(G, mmap_fn, 0, UPLOAD_CHUNK, 3, 0x1002, (u64)-1, 0);
        if ((s64)up_buf == -1) {
            up_buf = 0;
            NC(G, kclose, (u64)fd, 0,0,0,0,0);
            NC(G, send, (u64)client, (u64)RESP_500, (u64)str_len(RESP_500), 0, 0, 0);
            NC(G, close, (u64)client, 0,0,0,0,0);
            up_state.path[0] = 0;
            return 1;
        }
    }

    /* Body bytes that arrived with the headers go straight to the file. */
    u64 body0 = (u64)(n - he);
    u64 off = 0;
    while (off < body0) {
        s32 w = (s32)NC(G, kwrite, (u64)fd, (u64)(req + he + off),
                        body0 - off, 0, 0, 0);
        if (w <= 0) {
            up_state.file_fd = fd;
            up_state.client_fd = client;
            upload_abort(G, kopen, kclose, kunlink, "initial write");
            return 1;
        }
        off += (u64)w;
    }

    up_state.client_fd = client;
    up_state.file_fd = fd;
    up_state.remaining = clen - body0;
    up_state.idle_calls = 0;

    klog("upload: ");
    klog(up_state.name);
    klog(" -> /temp0/roms/\n");
    return 1;
}

/* Bounded slice of an in-progress upload. Called at the top of web_handle so
   pad POSTs queued behind it still drain in the same call. */
static void upload_pump(void *G, void *poll, void *recv, void *send,
                        void *kopen, void *kwrite, void *kclose,
                        void *kunlink) {
    int progressed = 0;
    for (int it = 0; it < UPLOAD_ITERS && up_state.remaining > 0; it++) {
        if (!poll_ready(G, poll, up_state.client_fd, 0)) break;
        u64 want = up_state.remaining < UPLOAD_CHUNK ?
                   up_state.remaining : UPLOAD_CHUNK;
        s32 n = (s32)NC(G, recv, (u64)up_state.client_fd, (u64)up_buf,
                        want, 0x80, 0, 0);
        if (n <= 0) { upload_abort(G, kopen, kclose, kunlink, "recv"); return; }
        u64 off = 0;
        while (off < (u64)n) {
            s32 w = (s32)NC(G, kwrite, (u64)up_state.file_fd,
                            (u64)(up_buf + off), (u64)n - off, 0, 0, 0);
            if (w <= 0) { upload_abort(G, kopen, kclose, kunlink, "write"); return; }
            off += (u64)w;
        }
        up_state.remaining -= (u64)n;
        progressed = 1;
    }

    if (progressed) up_state.idle_calls = 0;
    else if (++up_state.idle_calls > UPLOAD_STALL) {
        upload_abort(G, kopen, kclose, kunlink, "stall");
        return;
    }

    if (up_state.remaining == 0) {
        NC(G, kclose, (u64)up_state.file_fd, 0,0,0,0,0);
        NC(G, send, (u64)up_state.client_fd, (u64)RESP_200,
           (u64)str_len(RESP_200), 0, 0, 0);
        NC(G, kclose, (u64)up_state.client_fd, 0,0,0,0,0);
        roms_received++;
        klog("upload: complete, picker will list it on next open\n");
        upload_reset();
    }
}

/* ------------------------------------------------------------ web pad ----
 * (existing controller-page / pad-POST serving continues below) */

static int poll_ready(void *G, void *poll, s32 fd, s32 timeout_ms) {
    u8 pfd[8];
    *(s32 *)pfd = fd;
    *(u16 *)(pfd + 4) = 0x0001;   /* POLLIN */
    *(u16 *)(pfd + 6) = 0;
    return (s32)NC(G, poll, (u64)pfd, 1, (u64)timeout_ms, 0, 0, 0) > 0;
}

static int parse_pad_last(u8 *buf, s32 len) {
    int val = -1;
    for (s32 i = len - 2; i >= 1; i--) {
        if (buf[i] == '/' && buf[i + 1] == 'b') {
            val = 0;
            for (s32 j = i + 2; j < len && j < i + 8; j++) {
                if (buf[j] >= '0' && buf[j] <= '9') val = val * 10 + (buf[j] - '0');
                else break;
            }
            break;
        }
    }
    return val;
}

static int count_posts(u8 *buf, s32 len) {
    int c = 0;
    for (s32 i = 0; i < len - 4; i++)
        if (buf[i] == 'P' && buf[i+1] == 'O' && buf[i+2] == 'S' && buf[i+3] == 'T') c++;
    return c;
}

/* Adapter: native_call uses core.h's u64 (unsigned long); the link layer's
   nc type uses unsigned long long (LP64-clean and Windows-safe). Both are
   64-bit on the PS5 target, but the wrapper keeps the types honest. */
static link_ps5_u64 link_nc_adapter(void *gadget, void *fn,
                                     link_ps5_u64 a0, link_ps5_u64 a1,
                                     link_ps5_u64 a2, link_ps5_u64 a3,
                                     link_ps5_u64 a4, link_ps5_u64 a5) {
    return (link_ps5_u64)native_call(gadget, fn, (u64)a0, (u64)a1, (u64)a2,
                                     (u64)a3, (u64)a4, (u64)a5);
}

/* Serves the controller page, drains any pending button POSTs, and pumps an
   in-progress ROM upload. Returns non-zero when pad_out was updated. */
static u8 web_handle(void *G, void *poll, void *accept, void *recv,
                     void *send, void *close, void *sso, s32 listen_fd,
                     s32 *keep_fd, u8 *page, u64 page_len,
                     void *kopen, void *kwrite, void *kmkdir, void *kunlink,
                     void *mmap_fn, u16 *pad_out) {
    u8 got_input = 0;
    u8 req[512];

    /* An in-progress ROM upload gets its bounded slice first, so pad POSTs
       queued behind it still drain in the same call. */
    if (up_state.client_fd >= 0)
        upload_pump(G, poll, recv, send, kopen, kwrite, close, kunlink);

    if (*keep_fd >= 0) {
        for (int r = 0; r < 8; r++) {
            if (!poll_ready(G, poll, *keep_fd, 0)) break;
            s32 n = (s32)NC(G, recv, (u64)*keep_fd, (u64)req, 512, 0x80, 0, 0);
            if (n <= 0) { NC(G, close, (u64)*keep_fd, 0,0,0,0,0); *keep_fd = -1; break; }
            int v = parse_pad_last(req, n);
            if (v >= 0) {
                *pad_out = (u16)v;
                got_input = 1;
                int np = count_posts(req, n);
                for (int k = 0; k < np; k++)
                    NC(G, send, (u64)*keep_fd, (u64)RESP_204K, (u64)str_len(RESP_204K), 0, 0, 0);
            }
        }
    }

    for (int i = 0; i < 4; i++) {
        if (!poll_ready(G, poll, listen_fd, 0)) break;

        u8 sa[16]; s32 sa_len = 16;
        s32 client = (s32)NC(G, accept, (u64)listen_fd, (u64)sa, (u64)&sa_len, 0, 0, 0);
        if (client < 0) break;

        if (sso) { s32 one = 1; NC(G, sso, (u64)client, 6, 1, (u64)&one, 4, 0); }

        if (!poll_ready(G, poll, client, 0)) {
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
            continue;
        }

        s32 n = (s32)NC(G, recv, (u64)client, (u64)req, 512, 0x80, 0, 0);

        if (n > 11 && req[0] == 'P' && req[1] == 'O' && req[2] == 'S' &&
            req[3] == 'T' && req[4] == ' ' && req[5] == '/' &&
            req[6] == 'r' && req[7] == 'o' && req[8] == 'm' && req[9] == '?') {
            /* POST /rom?name=<file> -- OFW ROM upload. upload_begin always
               consumes the client (close or takes ownership). */
            upload_begin(G, send, close, kopen, kwrite, close,
                         kmkdir, kunlink, mmap_fn, client, req, n);
        } else if (n > 20 && req[0] == 'P' && req[1] == 'O' && req[2] == 'S' &&
                   req[3] == 'T' && req[4] == ' ' && req[5] == '/' &&
                   req[6] == 'r' && req[7] == 'o' && req[8] == 'm' &&
                   req[9] == 's' && req[10] == '_' && req[11] == 'b' &&
                   req[12] == 'e' && req[13] == 'g' && req[14] == 'i' &&
                   req[15] == 'n') {
            /* POST /roms_begin?count=N -- launcher announces a batch of N
               ROM uploads. The no-ROM wait loop shows progress and opens
               the picker only after all N arrive. */
            int cnt = 0;
            for (s32 i = 16; i < n; i++) {
                if (req[i] >= '0' && req[i] <= '9')
                    cnt = cnt * 10 + (req[i] - '0');
                else if (req[i] == ' ' || req[i] == '&' ||
                         req[i] == '\r' || req[i] == '\n')
                    break;
            }
            if (cnt > 0 && cnt <= 4096) {
                roms_expected = cnt;
                roms_received = 0;
                klog("roms_begin: batch announced\n");
            }
            NC(G, send, (u64)client, (u64)RESP_200,
               (u64)str_len(RESP_200), 0, 0, 0);
            NC(G, close, (u64)client, 0,0,0,0,0);
        } else if (n > 7 && req[0] == 'P' && req[5] == '/' && req[6] == 'b') {
            int v = parse_pad_last(req, n);
            if (v >= 0) { *pad_out = (u16)v; got_input = 1; }
            NC(G, send, (u64)client, (u64)RESP_204K, (u64)str_len(RESP_204K), 0, 0, 0);
            if (*keep_fd >= 0) NC(G, close, (u64)*keep_fd, 0,0,0,0,0);
            *keep_fd = client;
        } else if (n > 5 && req[0] == 'G' && req[4] == '/') {
            u64 off = 0;
            while (off < page_len) {
                u64 chunk = page_len - off;
                if (chunk > 2048) chunk = 2048;
                NC(G, send, (u64)client, (u64)(page + off), chunk, 0, 0, 0);
                off += chunk;
            }
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
        } else if (n > 0 && req[0] == 'O') {
            NC(G, send, (u64)client, (u64)RESP_CORS, (u64)str_len(RESP_CORS), 0, 0, 0);
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
        } else {
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
        }
    }
    return got_input;
}

/* ---------------------------------------------------------- native pad --- */

/* DualSense button bits -> the GBA_BTN_* mask the emulator and the web page
   both speak. GBA_CMD_MENU is the out-of-band command. The face-button
   layout keeps v0.6 parity; L1 opens the pause menu, R1 is fast-forward
   (held), and the GBA shoulders move to L2/R2, which sit at
   bits 8/9 in the same report word as the observed L1=bit10/R1=bit11. */
static u16 ds_to_gba(u32 b) {
    u32 r = 0;
    if (b & 0x00004000) r |= GBA_BTN_A;       /* CROSS            */
    if (b & 0x00002000) r |= GBA_BTN_B;       /* CIRCLE           */
    if (b & 0x00000004) r |= GBA_BTN_SELECT;  /* CREATE           */
    if (b & 0x00000008) r |= GBA_BTN_START;   /* OPTIONS          */
    if (b & 0x00000010) r |= GBA_BTN_UP;
    if (b & 0x00000040) r |= GBA_BTN_DOWN;
    if (b & 0x00000080) r |= GBA_BTN_LEFT;
    if (b & 0x00000020) r |= GBA_BTN_RIGHT;
    if (b & 0x00000100) r |= GBA_BTN_L;       /* L2 -> L          */
    if (b & 0x00000200) r |= GBA_BTN_R;       /* R2 -> R          */
    if (b & 0x00000400) r = GBA_CMD_MENU;     /* L1 -> pause menu */
    /* R1 (0x800) is NOT mapped here: it is the fast-forward hold, checked
       directly from last_pad_raw in the game loop. It must not overwrite
       the GBA buttons held alongside it. */
    return (u16)r;
}

/* Also hands back the raw button word. A command button that ends the session
   is worth being able to prove after the fact rather than infer. */
static u32 last_pad_raw;

static s32 read_native_pad(void *G, void *pad_read, s32 pad_h, u8 *pbuf) {
    if (pad_h < 0 || !pad_read) return -1;
    for (int i = 0; i < 128; i++) pbuf[i] = 0;
    s32 n = (s32)NC(G, pad_read, (u64)pad_h, (u64)pbuf, 1, 0, 0, 0);
    if (n <= 0 || (u32)n >= 0x80000000) return -1;
    u32 raw = *(u32 *)pbuf;
    if (raw & 0x80000000) return -1;
    last_pad_raw = raw & 0x001FFFFF;
    return (s32)ds_to_gba(last_pad_raw);
}

/* ------------------------------------------------------- ROM discovery -- */

/* Reads the 12-byte game title at ROM header offset 0xA0. Called AFTER the
   directory scan completes (fds closed) -- never during getdents iteration,
   where interleaved opens broke discovery on hardware (v1.2.0 regression).
   Uppercases for the caps-only UI font. Leaves the filename-based display
   untouched when the header is unreadable or the title is unusable. */
/* Formats a ROM header title (e.g., "POKEMON FIRERED") into a display name
   (e.g., "Pokemon Firered"). Title-cases each word. Special case: "POKEMON"
   becomes "Pokémon" with the accent. */
static void format_rom_name(const char *src, char *dst, int dst_size) {
    int si = 0, di = 0;
    int new_word = 1;
    while (src[si] && di < dst_size - 1) {
        char c = src[si++];
        if (c == ' ' || c == '_' || c == '-') {
            dst[di++] = ' ';
            new_word = 1;
        } else if (new_word) {
            /* Check for POKEMON -> Pokémon */
            if ((c == 'P' || c == 'p') &&
                (src[si] == 'O' || src[si] == 'o') &&
                (src[si+1] == 'K' || src[si+1] == 'k')) {
                /* Verify it's POKEMON */
                const char *p = src + si - 1;
                if ((p[0]=='P'||p[0]=='p') && (p[1]=='O'||p[1]=='o') &&
                    (p[2]=='K'||p[2]=='k') && (p[3]=='E'||p[3]=='e') &&
                    (p[4]=='M'||p[4]=='m') && (p[5]=='O'||p[5]=='o') &&
                    (p[6]=='N'||p[6]=='n')) {
                    /* Write "Pokémon" (UTF-8: é is 0xC3 0xA9) */
                    const char *poke = "Pok\xC3\xA9mon";
                    for (int k = 0; poke[k] && di < dst_size - 1; k++)
                        dst[di++] = poke[k];
                    si += 6; /* skip "OKEMON" (P already consumed) */
                    new_word = 0;
                    continue;
                }
            }
            /* Check for FIRERED -> FireRed */
            if ((c == 'F' || c == 'f') &&
                (src[si] == 'I' || src[si] == 'i') &&
                (src[si+1] == 'R' || src[si+1] == 'r')) {
                const char *p = src + si - 1;
                if ((p[0]=='F'||p[0]=='f') && (p[1]=='I'||p[1]=='i') &&
                    (p[2]=='R'||p[2]=='r') && (p[3]=='E'||p[3]=='e') &&
                    (p[4]=='R'||p[4]=='r') && (p[5]=='E'||p[5]=='e') &&
                    (p[6]=='D'||p[6]=='d')) {
                    const char *fr = "FireRed";
                    for (int k = 0; fr[k] && di < dst_size - 1; k++)
                        dst[di++] = fr[k];
                    si += 6; /* skip "IRERED" (F already consumed) */
                    new_word = 0;
                    continue;
                }
            }
            /* Check for LEAFGREEN -> LeafGreen */
            if ((c == 'L' || c == 'l') &&
                (src[si] == 'E' || src[si] == 'e') &&
                (src[si+1] == 'A' || src[si+1] == 'a')) {
                const char *p = src + si - 1;
                if ((p[0]=='L'||p[0]=='l') && (p[1]=='E'||p[1]=='e') &&
                    (p[2]=='A'||p[2]=='a') && (p[3]=='F'||p[3]=='f') &&
                    (p[4]=='G'||p[4]=='g') && (p[5]=='R'||p[5]=='r') &&
                    (p[6]=='E'||p[6]=='e') && (p[7]=='E'||p[7]=='e') &&
                    (p[8]=='N'||p[8]=='n')) {
                    const char *lg = "LeafGreen";
                    for (int k = 0; lg[k] && di < dst_size - 1; k++)
                        dst[di++] = lg[k];
                    si += 8; /* skip "EAFGREEN" (L already consumed) */
                    new_word = 0;
                    continue;
                }
            }
            dst[di++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
            /* Check if the word we just started is a Roman numeral (II, III,
               IV, etc.). If so, keep the rest uppercase. */
            {
                int wi = si - 1; /* start of current word */
                int wlen = 0;
                int is_roman = 1;
                while (src[wi + wlen] && src[wi + wlen] != ' ' &&
                       src[wi + wlen] != '_' && src[wi + wlen] != '-') {
                    char wc = src[wi + wlen];
                    if (wc != 'I' && wc != 'i' && wc != 'V' && wc != 'v' &&
                        wc != 'X' && wc != 'x' && wc != 'L' && wc != 'l' &&
                        wc != 'C' && wc != 'c' && wc != 'D' && wc != 'd' &&
                        wc != 'M' && wc != 'm') {
                        is_roman = 0;
                        break;
                    }
                    wlen++;
                    if (wlen > 4) { is_roman = 0; break; }
                }
                /* Single 'I' is a word, not a numeral (e.g., "I" as in...).
                   Require length >= 2 or it's a known single (V, X). */
                if (is_roman && wlen >= 2) {
                    /* Copy the rest of the numeral in uppercase. */
                    while (src[si] && src[si] != ' ' && src[si] != '_' &&
                           src[si] != '-' && di < dst_size - 1) {
                        char rc = src[si++];
                        dst[di++] = (rc >= 'a' && rc <= 'z') ? rc - 32 : rc;
                    }
                }
            }
            new_word = 0;
        } else {
            dst[di++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        }
    }
    dst[di] = 0;
}
static void rom_title_post_scan(void *G, void *kopen, void *kread,
                                void *kclose, struct rom_entry *roms,
                                int count) {
    if (!kopen || !kread || !kclose) return;
    for (int i = 0; i < count; i++) {
        s32 fd = (s32)NC(G, kopen, (u64)roms[i].filename, 0, 0, 0, 0, 0);
        if (fd < 0) continue;
        u8 hdr[192];
        s32 n = (s32)NC(G, kread, (u64)(u32)fd, (u64)hdr, 192, 0, 0, 0);
        NC(G, kclose, (u64)(u32)fd, 0, 0, 0, 0, 0);
        if (n < 0xAC) continue;
        int len = 12;
        while (len > 0 && hdr[0xA0 + len - 1] == ' ') len--;
        if (len <= 0) continue;
        int ok = 1;
        for (int k = 0; k < len; k++) {
            u8 ch = hdr[0xA0 + k];
            if (ch < 0x20 || ch > 0x7E) { ok = 0; break; }
        }
        if (!ok) continue;
        int o = 0;
        for (int k = 0; k < len && o < MAX_NAME - 1; k++) {
            char c = (char)hdr[0xA0 + k];
            if (c >= 'a' && c <= 'z') c -= 32;
            roms[i].display[o++] = c;
        }
        roms[i].display[o] = 0;
    }
}

/* Appends every ROM in one directory to the list, skipping duplicates. Each
   entry carries its full path, because ROMs are gathered from more than one
   directory and the picker has to be able to open any of them. */
static int scan_dir(void *G, void *kopen, void *kclose, void *getdents,
                    void *mmap_fn, void *munmap_fn,
                    const char *dir, struct rom_entry *roms, int count) {
    if (!kopen || !getdents) return count;

    s32 dfd = (s32)NC(G, kopen, (u64)dir, 0x20000 /* O_DIRECTORY */, 0, 0, 0, 0);
    if (dfd < 0) return count;

    u8 *dbuf = (u8 *)NC(G, mmap_fn, 0, 0x2000, 3, 0x1002, (u64)-1, 0);
    if ((s64)dbuf == -1) {
        NC(G, kclose, (u64)dfd, 0,0,0,0,0);
        return count;
    }

    int dirlen = str_len(dir);

    for (;;) {
        s32 nread = (s32)NC(G, getdents, (u64)dfd, (u64)dbuf, 0x2000, 0, 0, 0);
        if (nread <= 0) break;

        int off = 0;
        while (off < nread && count < MAX_ROMS) {
            u16 reclen = *(u16 *)(dbuf + off + 4);
            u8  namlen = *(u8 *)(dbuf + off + 7);
            char *name = (char *)(dbuf + off + 8);
            if (reclen == 0) break;

            if (namlen > 0 && is_rom_file(name)) {
                char full[128];
                int p = 0;
                for (int i = 0; i < dirlen && p < 120; i++) full[p++] = dir[i];
                for (int i = 0; name[i] && p < 127; i++) full[p++] = name[i];
                full[p] = 0;

                int dup = 0;
                for (int j = 0; j < count && !dup; j++) {
                    int match = 1;
                    for (int c = 0; c < 128; c++) {
                        if (roms[j].filename[c] != full[c]) { match = 0; break; }
                        if (!full[c]) break;
                    }
                    dup = match;
                }

                if (!dup) {
                    for (int c = 0; c <= p; c++) roms[count].filename[c] = full[c];
                    extract_rom_name(name, roms[count].display, MAX_NAME);
                    count++;
                }
            }
            off += reclen;
        }
        if (count >= MAX_ROMS) break;
    }

    if (munmap_fn) NC(G, munmap_fn, (u64)dbuf, 0x2000, 0,0,0,0);
    NC(G, kclose, (u64)dfd, 0,0,0,0,0);
    return count;
}

/* Savedata ROMs first, then the OFW upload target /temp0/roms/. The full-path
   dedup in scan_dir keeps the two from ever colliding. /temp0 is wiped on
   reboot; the picker simply shows whatever is there when it opens. */
static int scan_all_dirs(void *G, void *kopen, void *kclose,
                         void *getdents, void *mmap_fn, void *munmap_fn,
                         struct rom_entry *roms, int count) {
    count = scan_dir(G, kopen, kclose, getdents, mmap_fn, munmap_fn, ROM_DIR,
                     roms, count);
    return scan_dir(G, kopen, kclose, getdents, mmap_fn, munmap_fn,
                    TEMP_ROM_DIR, roms, count);
}

/* --------------------------------------------------------- pause menu ---- */

/* Everything the pause menu loop needs from _start's locals. The game stays
   loaded while the menu is open: gba_run_frame() is simply not called, so
   opening the menu never resets the game. */
struct pause_ctx {
    void *G;
    void *pad_read; s32 pad_h; u8 *pad_buf;
    int has_web;
    void *web_handle; void *poll; void *accept; void *recvfrom; void *sendto;
    void *kclose; void *setsockopt_fn;
    s32 web_fd; s32 *web_client;
    u8 *web_page; u32 web_len;
    void *kopen; void *kwrite; void *kmkdir; void *kunlink; void *mmap;
    void **fbs; int *active;
    s32 video; void *vid_flip;
    u64 eq; void *wait_eq; void *usleep;
    const char *rom_title;
    u16 *web_pad; int *input_src;
    u32 *total_frames;
};

/* Pause overlay: dims the last game frame, draws the menu panel over it, and
   handles navigation with soft click feedback. Returns MENU_RESUME,
   MENU_ROMS or MENU_QUIT. Before returning it waits for L1 release, so the
   press that opened the menu cannot toggle it straight back open. */
/* Sets the pause-menu toast text and its frame counter. */
static void show_toast(char *dst, int *frames, const char *msg) {
    int i = 0;
    while (msg[i] && i < 15) { dst[i] = msg[i]; i++; }
    dst[i] = 0;
    *frames = 75;
}

/* Pixel-style fast-forward indicator drawn into the 240x160 GBA framebuffer
   before scaling: two right-pointing triangles (>>) plus "2X" or "4X" in a
   3x5 pixel font, top-right corner. Scales with the game for an authentic
   pixel look. */
static void draw_ff_indicator(int level) {
    /* 4x7 right triangle. */
    static const char *tri[7] = {
        "#...", "##..", "###.", "####", "###.", "##..", "#...",
    };
    /* 3x5 pixel font for '2', '4', 'X'. */
    static const char *f2[5] = { "###", "..#", "###", "#..", "###", };
    static const char *f4[5] = { "#.#", "#.#", "###", "..#", "..#", };
    static const char *fx[5] = { "#.#", "#.#", ".#.", "#.#", "#.#", };
    const char **digit = (level == 1) ? f2 : f4;

    int x0 = 240 - 20 - 3;  /* 20px wide, 3px margin */
    int y0 = 3;
    u32 col = 0xFFFFFFFFu;

    /* Two triangles. */
    for (int t = 0; t < 2; t++) {
        int tx = x0 + t * 6;
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 4; c++) {
                if (tri[r][c] == '#')
                    gba_framebuffer[(y0 + r) * 240 + (tx + c)] = col;
            }
        }
    }
    /* "2X" or "4X" in 3x5 font, vertically centered. */
    for (int ch = 0; ch < 2; ch++) {
        const char **g = (ch == 0) ? digit : fx;
        int gx = x0 + 13 + ch * 4;
        for (int r = 0; r < 5; r++) {
            for (int c = 0; c < 3; c++) {
                if (g[r][c] == '#')
                    gba_framebuffer[(y0 + 1 + r) * 240 + (gx + c)] = col;
            }
        }
    }
}

static int run_pause_menu(struct pause_ctx *c) {
    int cursor = 0, toast = 0, anim = 0, hold = 0;
    int last_cursor = -1, last_toast = -1, last_volume = -1, first = 1;
    char toast_msg[16];
    toast_msg[0] = 0;
    /* Start as if every button were held: the L1 press that opened the menu
       is still down on the first frame and must not read as a fresh press.
       menu_held tracks L1 separately, because GBA_CMD_MENU is a command
       word, not a button mask, and never appears in `pressed`. */
    u16 prev_btn = 0xFFFF;
    int menu_held = 1;
    int action = -1;
    /* The menu only opens on an L1 edge, so L1 is physically down on entry.
       Seed the last-good button word with that known-true state: the first
       scePadReadState poll after the play loop's poll reliably fails
       (polled too soon -- the pad needs milliseconds between polls), and a
       failed poll must hold the last state, never fake "all released". */
    u16 last_good_btn = GBA_CMD_MENU;
    char last_link[32]; last_link[0] = 0;
    {
        char lb[64];
        snprintf(lb, sizeof lb, "menu: open (input_src=%d)\n", *c->input_src);
        klog(lb);
    }

    gba_ui_click(CLICK_CONFIRM);

    for (;;) {
        u16 wb = 0; int web_got = 0;
        bridge_poll();  /* link-cable pump: answer the joiner's REQs, update status */
        if (c->has_web) {
            web_got = web_handle(c->G, c->poll, c->accept, c->recvfrom,
                                 c->sendto, c->kclose, c->setsockopt_fn,
                                 c->web_fd, c->web_client, c->web_page,
                                 c->web_len, c->kopen, c->kwrite, c->kmkdir,
                                 c->kunlink, c->mmap, &wb);
            if (web_got) *c->web_pad = wb;
        }

        s32 nb = read_native_pad(c->G, c->pad_read, c->pad_h, c->pad_buf);
        u16 btn;

        /* A failed poll is "no new information", not input. Hold the last
           good word (seeded to GBA_CMD_MENU: L1 is down on menu entry). */
        if (*c->input_src == 2) {
            btn = *c->web_pad;
        } else if (nb < 0) {
            btn = last_good_btn;
        } else if (*c->input_src == 0) {
            btn = 0;
            if (nb > 0 && nb < GBA_CMD_MENU) {
                *c->input_src = 1; btn = (u16)nb;
            } else if (web_got && *c->web_pad > 0 && *c->web_pad < GBA_CMD_MENU) {
                *c->input_src = 2; btn = *c->web_pad;
            }
            if (web_got && *c->web_pad >= GBA_CMD_MENU) btn = *c->web_pad;
            if (nb >= GBA_CMD_MENU) btn = (u16)nb;
            last_good_btn = btn;
        } else {
            btn = (u16)nb;
            last_good_btn = btn;
        }
        if (btn >= GBA_CMD_MENU) *c->web_pad = 0;

        /* (R1-hold quit removed in v1.2.12: R1 is now fast-forward in-game.) */

        u16 pressed = 0;

        if (btn == GBA_CMD_MENU) {
            /* L1 toggles the menu closed. The command word is not a button
               mask (its bits overlap real buttons), so it is tested for
               equality and never fed to the navigator below. menu_held
               swallows the press that opened the menu. */
            if (!menu_held) {
                gba_ui_click(CLICK_BACK);
                action = MENU_RESUME;
                klog("menu: close by L1 toggle\n");
            }
        } else {
            if (menu_held) {
                /* First frame after the opening L1 is released: any buttons
                   already down (held over from gameplay) count as held,
                   not fresh presses. */
                prev_btn = 0xFFFF;
                menu_held = 0;
            }
            pressed = btn & ~prev_btn;
            int move = 0;
            if (btn & GBA_BTN_UP) {
                hold++;
                if ((pressed & GBA_BTN_UP) || (hold > 12 && hold % 4 == 0)) move = -1;
            } else if (btn & GBA_BTN_DOWN) {
                hold++;
                if ((pressed & GBA_BTN_DOWN) || (hold > 12 && hold % 4 == 0)) move = 1;
            } else {
                hold = 0;
            }

            /* VOLUME slider: LEFT/RIGHT on the VOLUME item (options level). */
            if (g_menu_level == 1 && cursor == 2) {
                if (pressed & GBA_BTN_LEFT) {
                    if (g_volume > 0) {
                        g_volume--;
                        gba_ui_click(CLICK_TICK);
                    }
                } else if (pressed & GBA_BTN_RIGHT) {
                    if (g_volume < 10) {
                        g_volume++;
                        gba_ui_click(CLICK_TICK);
                    }
                }
            }

            if (move) {
                int nitems = (g_menu_level == 0) ? MENU_NITEMS : OPT_NITEMS;
                cursor += move;
                if (cursor < 0) cursor = nitems - 1;
                if (cursor >= nitems) cursor = 0;
                gba_ui_click(CLICK_TICK);
            }
            if ((pressed & GBA_BTN_A) || (pressed & GBA_BTN_START)) {
                gba_ui_click(CLICK_CONFIRM);
                if (g_menu_level == 0) {
                    /* Main menu: RESUME (or NO ROM LOADED), LOAD ROM,
                       SAVE STATE, LOAD STATE, OPTIONS, LINK, QUIT
                       (always last). */
                    if (cursor == 0) {
                        /* RESUME only works when a game is loaded. When no
                           ROM is loaded, this item shows "NO ROM LOADED"
                           and does nothing. */
                        if (c->rom_title && c->rom_title[0])
                            action = MENU_RESUME;
                    }
                    else if (cursor == 1) action = MENU_ROMS;
                    else if (cursor == 2) {
                        show_toast(toast_msg, &toast,
                                   gba_state_save() == 0 ? "STATE SAVED"
                                                         : "STATE FAILED");
                    }
                    else if (cursor == 3) {
                        show_toast(toast_msg, &toast,
                                   gba_state_load() == 0 ? "STATE LOADED"
                                                         : "NO STATE");
                    }
                    else if (cursor == 4) {
                        g_menu_level = 1;
                        cursor = 0;
                    }
                    else if (cursor == 5) {
                        /* LINK toggle: host when off, disconnect when on.
                           The driver is attached at ROM load and no-ops
                           until a session exists. */
                        if (bridge_role() == LINK_ROLE_NONE) {
                            if (bridge_host(0))
                                show_toast(toast_msg, &toast,
                                           "LINK HOSTING");
                            else
                                show_toast(toast_msg, &toast,
                                           "LINK FAILED");
                        } else {
                            /* Disconnect: end the UDP session but KEEP the
                             * SIO driver attached, so re-hosting later
                             * works without a ROM reload. */
                            bridge_disconnect();
                            show_toast(toast_msg, &toast, "LINK OFF");
                        }
                    }
                    else action = MENU_QUIT;
                } else {
                    /* Options submenu: SCREEN, COLOR, VOLUME, SAVE GAME, BACK. */
                    if (cursor == 0) {
                        g_fullscreen = !g_fullscreen;
                        show_toast(toast_msg, &toast,
                                   g_fullscreen ? "SCREEN: 16:9" : "SCREEN: 3:2");
                    }
                    else if (cursor == 1) {
                        g_accent_idx = (g_accent_idx + 1) % ACCENT_NCOLORS;
                        char cmsg[16];
                        {
                            const char *p = "COLOR: ";
                            const char *n = accent_names[g_accent_idx];
                            int k = 0;
                            while (*p && k < 15) cmsg[k++] = *p++;
                            while (*n && k < 15) cmsg[k++] = *n++;
                            cmsg[k] = 0;
                        }
                        show_toast(toast_msg, &toast, cmsg);
                    }
                    else if (cursor == 2) {
                        /* VOLUME: adjusted with LEFT/RIGHT, not SELECT. */
                    }
                    else if (cursor == 3) {
                        gba_save_store();
                        show_toast(toast_msg, &toast, "SAVED");
                    }
                    else {
                        g_menu_level = 0;
                        cursor = 4;  /* back to OPTIONS */
                    }
                }
            }
            if (pressed & GBA_BTN_B) {
                gba_ui_click(CLICK_BACK);
                if (g_menu_level == 1) {
                    g_menu_level = 0;
                    cursor = 4;  /* back to OPTIONS */
                } else {
                    action = MENU_RESUME;
                }
            }
        }
        prev_btn = btn;

        /* The game frame is frozen while the core is paused. Dim it at GBA
           resolution into cached RAM (menu_dim) -- only while the 10-frame
           fade runs, then never again -- and composite on demand, only when
           the cursor, toast, or fade changed. Every video-memory access in
           this path is a write: clear, scale-up, opaque menu overlay.
           Nothing reads the video mapping, because reads from that
           write-combining mapping were the v1.1.0-v1.1.2 menu lag (the ROM
           picker never lagged: its blit path is write-only). */
        int bg_changed = 0;
        if (anim < 10) { anim++; bg_changed = 1; }
        if (first) { first = 0; bg_changed = 1; }
        if (bg_changed) {
            for (int i = 0; i < GBA_W * GBA_H; i++)
                menu_dim[i] = gba_framebuffer[i];
            dim_buf(menu_dim, GBA_W * GBA_H, 40 + 60 * anim / 10);
        }
        int dirty = bg_changed || cursor != last_cursor || toast != last_toast ||
                    g_volume != last_volume ||
                    strcmp(bridge_status_text(), last_link) != 0;
        if (dirty) {
            *c->active ^= 1;
            u32 *dst = (u32 *)c->fbs[*c->active];
            clear_fb(dst);
            if (g_fullscreen) blit_fullscreen(dst, menu_dim);
            else blit_scale(dst, menu_dim);
            ui_clear(ui_screen);
            menu_draw(ui_screen, c->rom_title, cursor, toast_msg, toast);
            blit_ui_blend(dst, ui_screen);
            last_cursor = cursor;
            last_toast = toast;
            last_volume = g_volume;
            {
                const char *ls = bridge_status_text();
                int k = 0;
                while (ls[k] && k < 31) { last_link[k] = ls[k]; k++; }
                last_link[k] = 0;
            }
        }
        NC(c->G, c->vid_flip, (u64)(u32)c->video, (u64)(u32)*c->active, 1,
           *c->total_frames, 0, 0);
        if (c->eq && c->wait_eq) {
            u8 evt[64]; s32 cnt = 0;
            NC(c->G, c->wait_eq, c->eq, (u64)evt, 1, (u64)&cnt, 0, 0);
        }
        /* Drain queued click tones, at most 2 grains per frame: with the
           single-grain v1.1.2 clicks the queue never holds more than a tone
           or two, so input never stalls on audio. */
        gba_audio_flush_menu();
        (*c->total_frames)++;
        if (toast) toast--;

        if (action >= 0) {
            /* v1.1.4-dbg: log exactly what closed the menu. */
            char ab[64];
            snprintf(ab, sizeof ab, "menu: action=%d, leaving loop\n",
                     action);
            klog(ab);
            break;
        }
    }

    /* Wait for the menu button to be released on either input source. */
    for (;;) {
        u16 wb = 0; int wg = 0;
        if (c->has_web) {
            wg = web_handle(c->G, c->poll, c->accept, c->recvfrom, c->sendto,
                            c->kclose, c->setsockopt_fn, c->web_fd,
                            c->web_client, c->web_page, c->web_len, c->kopen,
                            c->kwrite, c->kmkdir, c->kunlink, c->mmap, &wb);
            if (wg) *c->web_pad = wb;
        }
        s32 nb = read_native_pad(c->G, c->pad_read, c->pad_h, c->pad_buf);
        /* A failed poll is not a release: only a confirmed not-held read
           breaks the wait, otherwise a too-soon poll would end the wait
           while L1 is still down and the play loop would re-open the menu. */
        int held;
        if (nb < 0) held = 1;
        else held = (nb == (s32)GBA_CMD_MENU) ||
                    (wg && wb == GBA_CMD_MENU) ||
                    (*c->web_pad == GBA_CMD_MENU);
        if (!held) break;
        /* Let the close/back click finish playing while we wait; without
           this it would leak into the resumed game's audio. */
        gba_audio_flush_menu();
        if (c->usleep) NC(c->G, c->usleep, 16000, 0, 0, 0, 0, 0);
    }
    *c->web_pad = 0;
    return action;
}

/* -------------------------------------------------------------- entry ---- */

__attribute__((section(".text._start")))
void _start(u64 eboot_base, u64 dlsym_addr, struct ext_args *ext) {
    void *G = (void *)(eboot_base + GADGET_OFFSET);
    void *D = (void *)dlsym_addr;
    ext->step = 1;

    /* Beacons for the window before shim_init, where klog() is not usable yet
       because it lives in the very .bss this code is about to map. ext->status
       and ext->step only come back if _start returns, so a hang in here would
       otherwise be completely silent -- and on this target a hang costs a
       reboot. Everything below uses arguments and stack only. */
    void *early_sendto = SYM(G, D, LIBKERNEL_HANDLE, "sendto");
    s32   early_fd = ext->log_fd;
    u8   *early_sa = ext->log_addr;

#define BEACON(m) do { \
        if (early_sendto && early_fd >= 0) \
            NC(G, early_sendto, (u64)early_fd, (u64)(m), sizeof(m) - 1, \
               0, (u64)early_sa, 16); \
    } while (0)

    BEACON("boot: _start entered\n");

    /* Must come first: until this returns, writing any global faults. */
    void *mmap = SYM(G, D, LIBKERNEL_HANDLE, "mmap");
    void *munmap = SYM(G, D, LIBKERNEL_HANDLE, "munmap");
    if (!mmap) {
        BEACON("boot: FAILED to resolve mmap\n");
        ext->status = -3; ext->step = 3; return;
    }
    BEACON("boot: mmap resolved, mapping data region\n");

    if (!boot_data_region(G, mmap, munmap, early_sendto, early_fd, early_sa)) {
        BEACON("boot: FAILED to map .data/.bss region\n");
        ext->status = -4; ext->step = 4; return;
    }
    BEACON("boot: data region live, globals now writable\n");
    ext->step = 2;

    void *usleep    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelUsleep");
    void *cancel    = SYM(G, D, LIBKERNEL_HANDLE, "scePthreadCancel");
    void *load_mod  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelLoadStartModule");
    void *alloc_dm  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelAllocateDirectMemory");
    void *map_dm    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelMapDirectMemory");
    void *dm_size   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelGetDirectMemorySize");
    void *create_eq = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelCreateEqueue");
    void *wait_eq   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWaitEqueue");
    void *delete_eq = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelDeleteEqueue");
    void *kopen     = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelOpen");
    void *kread     = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelRead");
    void *kwrite    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWrite");
    void *kclose    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelClose");
    void *kmkdir    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelMkdir");
    void *klseek    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelLseek");
    if (!klseek) klseek = SYM(G, D, LIBKERNEL_HANDLE, "lseek");
    void *gettod    = SYM(G, D, LIBKERNEL_HANDLE, "gettimeofday");
    void *recvfrom  = SYM(G, D, LIBKERNEL_HANDLE, "recvfrom");
    void *sendto    = SYM(G, D, LIBKERNEL_HANDLE, "sendto");
    void *accept    = SYM(G, D, LIBKERNEL_HANDLE, "accept");
    void *poll      = SYM(G, D, LIBKERNEL_HANDLE, "poll");
    void *setsockopt_fn  = SYM(G, D, LIBKERNEL_HANDLE, "setsockopt");
    void *link_socket_fn = SYM(G, D, LIBKERNEL_HANDLE, "socket");
    if (!link_socket_fn)
        link_socket_fn = SYM(G, D, LIBKERNEL_HANDLE, "sceNetSocket");
    void *link_bind_fn   = SYM(G, D, LIBKERNEL_HANDLE, "bind");
    if (!link_bind_fn)
        link_bind_fn = SYM(G, D, LIBKERNEL_HANDLE, "sceNetBind");
    void *getdents  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelGetdents");
    if (!getdents) getdents = SYM(G, D, LIBKERNEL_HANDLE, "getdents");
    void *kunlink   = SYM(G, D, LIBKERNEL_HANDLE, "unlink");

    s32 log_fd = ext->log_fd;
    u8 log_sa[16];
    for (int i = 0; i < 16; i++) log_sa[i] = ext->log_addr[i];

    s32 web_fd      = (s32)ext->dbg[0];
    u8 *web_page    = (u8 *)ext->dbg[1];
    u64 web_len     = ext->dbg[2];
    s32 userId      = (s32)ext->dbg[3];

    if (!usleep || !load_mod) { ext->status = -1; ext->step = 6; return; }

    /* The bump arena backs every malloc in the payload; the ROM alone can
     * need 32MB. But ps2emu does not always grant 64MB of anonymous VM in
     * one mapping (mmap then returns -1), so walk down a ladder of sizes
     * and take the largest that fits. Each attempt is logged with bmsg --
     * klog() is not up yet -- so the usable size is visible in the UDP log.
     * Below ~36MB large ROMs stop loading (their 32MB malloc fails
     * gracefully in gba_load_rom); below 16MB give up. */
    static const u64 arena_mb[] = { 64, 48, 40, 36, 34, 32, 24, 16 };
    void *arena = (void *)0;
    u64 arena_size = 0;
    for (u64 ti = 0; ti < sizeof(arena_mb) / sizeof(arena_mb[0]); ti++) {
        u64 try_size = arena_mb[ti] * 1024 * 1024;
        bmsg(G, early_sendto, early_fd, early_sa, "boot: arena try MB",
             arena_mb[ti]);
        void *got = (void *)NC(G, mmap, 0, try_size, 3, 0x1002, (u64)-1, 0);
        if ((s64)got != -1) {
            arena = got;
            arena_size = try_size;
            bmsg(G, early_sendto, early_fd, early_sa, "boot: arena MB ok",
                 arena_mb[ti]);
            break;
        }
    }
    if (!arena) { ext->status = -2; ext->step = 3; return; }

    struct shim_ps5 ps5;
    ps5.gadget = G;
    ps5.open   = kopen;
    ps5.read   = kread;
    ps5.write  = kwrite;
    ps5.close  = kclose;
    ps5.lseek  = klseek;
    ps5.mkdir  = kmkdir;
    ps5.sendto = sendto;
    ps5.gettimeofday = gettod;
    ps5.log_fd = log_fd;
    ps5.log_sa = log_sa;
    shim_init(&ps5, arena, arena_size);

    /* Link-cable bridge backend: install the resolved socket symbols once.
       The bridge stays dormant until HOST LINK is picked in the pause menu. */
    {
        struct link_ps5_fns lfns;
        lfns.nc = link_nc_adapter;
        lfns.gadget = G;
        lfns.socket_fn = link_socket_fn;
        lfns.bind_fn = link_bind_fn;
        lfns.recvfrom_fn = recvfrom;
        lfns.sendto_fn = sendto;
        lfns.poll_fn = poll;
        lfns.setsockopt_fn = setsockopt_fn;
        lfns.close_fn = kclose;
        link_ps5_install(&lfns);
    }

    klog("=== " VERSION_STR "\n");
    klog("boot: shim up, streaming log is live\n");
    install_fault_trap(G, D, sendto, log_fd, log_sa);

    /* Luac0re enters the payload through a ROP stack pivot, so this may not be
       the thread's real stack. Recording it lets a fault's si_addr be told
       apart: near this value means stack overflow, near __bss_end means a
       global overran. */
    {
        u64 rsp;
        __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
        bmsg(G, sendto, log_fd, log_sa, "boot: stack pointer", rsp);
    }

    s32 vid_mod = (s32)NC(G, load_mod, (u64)"libSceVideoOut.sprx", 0,0,0,0,0);
    s32 aud_mod = (s32)NC(G, load_mod, (u64)"libSceAudioOut.sprx", 0,0,0,0,0);

    void *vid_open  = SYM(G, D, vid_mod, "sceVideoOutOpen");
    void *vid_close = SYM(G, D, vid_mod, "sceVideoOutClose");
    void *vid_reg   = SYM(G, D, vid_mod, "sceVideoOutRegisterBuffers");
    void *vid_flip  = SYM(G, D, vid_mod, "sceVideoOutSubmitFlip");
    void *vid_rate  = SYM(G, D, vid_mod, "sceVideoOutSetFlipRate");
    void *vid_evt   = SYM(G, D, vid_mod, "sceVideoOutAddFlipEvent");
    void *aud_init  = SYM(G, D, aud_mod, "sceAudioOutInit");
    void *aud_open  = SYM(G, D, aud_mod, "sceAudioOutOpen");
    void *aud_out   = SYM(G, D, aud_mod, "sceAudioOutOutput");
    void *aud_vol   = SYM(G, D, aud_mod, "sceAudioOutSetVolume");
    void *aud_close = SYM(G, D, aud_mod, "sceAudioOutClose");

    ext->step = 5;

    /* Take the display away from ps2emu: stop its GS thread, then close the
       sceVideoOut handle it is holding. */
    if (cancel) {
        u64 gs = *(u64 *)(eboot_base + EBOOT_GS_THREAD);
        if (gs) NC(G, cancel, gs, 0,0,0,0,0);
    }
    NC(G, usleep, 300000, 0,0,0,0,0);

    s32 emu_vid = *(s32 *)(eboot_base + EBOOT_VIDOUT);
    if (vid_close && emu_vid >= 0) NC(G, vid_close, (u64)emu_vid, 0,0,0,0,0);
    NC(G, usleep, 100000, 0,0,0,0,0);

    klog("video: ps2emu GS thread stopped, opening our own output\n");
    s32 video = (s32)NC(G, vid_open, 0xFF, 0, 0, 0, 0, 0);
    if (video < 0) { ext->status = -10; ext->step = 11; return; }

    u64 eq = 0;
    if (create_eq) NC(G, create_eq, (u64)&eq, (u64)"gbaq", 0,0,0,0);
    if (vid_evt && eq) NC(G, vid_evt, eq, (u64)video, 0,0,0,0);

    u64 mem_total = dm_size ? NC(G, dm_size, 0,0,0,0,0,0) : 0x300000000ULL;
    u64 phys = 0;
    NC(G, alloc_dm, 0, mem_total, FB_TOTAL, 0x200000, 3, (u64)&phys);
    void *vmem = 0;
    NC(G, map_dm, (u64)&vmem, FB_TOTAL, 0x33, 0, phys, 0x200000);
    if (!vmem) { ext->status = -21; ext->step = 22; return; }

    u8 attr[64];
    for (int i = 0; i < 64; i++) attr[i] = 0;
    *(u32 *)(attr + 0)  = 0x80000000;
    *(u32 *)(attr + 4)  = 1;
    *(u32 *)(attr + 12) = SCR_W;
    *(u32 *)(attr + 16) = SCR_H;
    *(u32 *)(attr + 20) = SCR_W;

    void *fbs[2];
    fbs[0] = vmem;
    fbs[1] = (u8 *)vmem + FB_ALIGNED;

    if (NC(G, vid_reg, (u64)video, 0, (u64)fbs, 2, (u64)attr, 0) != 0) {
        ext->status = -30; ext->step = 30; return;
    }
    if (vid_rate) NC(G, vid_rate, (u64)video, 0, 0,0,0,0);
    clear_fb((u32 *)fbs[0]);
    clear_fb((u32 *)fbs[1]);

    NC(G, load_mod, (u64)"libSceUserService.sprx", 0,0,0,0,0);

    /* ps2emu may still hold audio handles; free them before claiming one. */
    if (aud_close)
        for (int h = 0; h < 8; h++) NC(G, aud_close, (u64)h, 0,0,0,0,0);

    /* Proven bring-up on PS5 (ps5-xash3d ran it on FW 12.02): init, open,
       then SetVolume. The port defaults to silent until the volume array
       is pushed -- eight int32 entries, 0x8000 = 0 dB, flags 3 = L+R. */
    if (aud_init) NC(G, aud_init, 0,0,0,0,0,0);

    s32 audio_h = -1;
    if (aud_open)
        audio_h = (s32)NC(G, aud_open, 0xFF, 0, 0, SAMPLES_PER_BUF, SAMPLE_RATE, AUDIO_S16_STEREO);

    s32 vol_ret = -1;
    if (audio_h >= 0 && aud_vol) {
        s32 vol[8];
        for (int i = 0; i < 8; i++) vol[i] = 0x8000;
        vol_ret = (s32)NC(G, aud_vol, (u64)(u32)audio_h, 3, (u64)vol, 0, 0, 0);
    }

    struct gba_audio_iface ai;
    ai.gadget    = G;
    ai.audio_out = aud_out;
    ai.handle    = audio_h;
    gba_glue_init(&ai);

    s32 pad_mod = (s32)NC(G, load_mod, (u64)"libScePad.sprx", 0,0,0,0,0);
    void *pad_init_fn = SYM(G, D, pad_mod, "scePadInit");
    void *pad_geth    = SYM(G, D, pad_mod, "scePadGetHandle");
    void *pad_read    = SYM(G, D, pad_mod, "scePadRead");
    if (pad_init_fn) NC(G, pad_init_fn, 0,0,0,0,0,0);
    s32 pad_h = -1;
    if (pad_geth) pad_h = (s32)NC(G, pad_geth, (u64)userId, 0, 0, 0, 0, 0);
    u8 pad_buf[128];

    klog(pad_h >= 0 ? "Native pad OK\n" : "Native pad N/A\n");
    {
        char ab[64];
        snprintf(ab, sizeof ab, "Audio out: handle=%d vol_ret=%d\n",
                 audio_h, vol_ret);
        klog(ab);
    }

    klog("video, audio and pad are up -- scanning for ROMs\n");

    /* ------------------------------------------------------ ROM listing -- */

    struct rom_entry *roms = (struct rom_entry *)NC(G, mmap, 0,
        sizeof(struct rom_entry) * MAX_ROMS, 3, 0x1002, (u64)-1, 0);
    int rom_count = 0;

    /* Resolve the savedata mount primitive. Writing is done in short
       read-write windows around each save, not by holding one open. */
    savedata_init(G, D, load_mod, eboot_base, userId);

    /* Harmless if the directories already exist (EEXIST). Logged because
       a failed SAVE_DIR mkdir is the only way a later save open can die
       with ENOENT despite O_CREAT. */
    if (kmkdir) {
        s32 mr = (s32)NC(G, kmkdir, (u64)ROM_DIR,  0x1FF, 0, 0, 0, 0);
        s32 ms = (s32)NC(G, kmkdir, (u64)SAVE_DIR, 0x1FF, 0, 0, 0, 0);
        s32 mt = (s32)NC(G, kmkdir, (u64)TEMP_ROM_DIR, 0x1FF, 0, 0, 0, 0);
        char mb[96];
        snprintf(mb, sizeof mb, "dirs: mkdir rom=%d save=%d tmp=%d\n",
                 mr, ms, mt);
        klog(mb);
    }

    /* ROMs come from two places: the savedata container (jailbroken setups,
     * put there with a save manager) and /temp0/roms/ (OFW, uploaded over the
     * network with POST /rom or upload_rom.py).
     *
     * There used to be an FTP server here, listening on 1337 so ROMs could be
     * pushed over the network. It is gone, replaced by the HTTP upload
     * endpoint in web_handle: FTP never removed a dependency (getting
     * Luac0re into the container already required a save manager), it only
     * added a second way for the launch path to go wrong and a wait to sit
     * through on every boot. The upload endpoint needs no extra port and no
     * extra client -- the controller page at :9030 already speaks HTTP. */
    if ((s64)roms != -1) {
        rom_count = scan_all_dirs(G, kopen, kclose, getdents, mmap, munmap,
                                  roms, rom_count);
        /* Internal titles, read after the scan (fds closed). */
        rom_title_post_scan(G, kopen, kread, kclose, roms, rom_count);
    }

    { char msg[32]; int k = 0; int v = rom_count;
      const char *lbl = "ROMs found: ";
      while (*lbl) msg[k++] = *lbl++;
      if (!v) msg[k++] = 48;
      else { char t[12]; int m = 0;
             while (v) { t[m++] = (char)(48 + v % 10); v /= 10; }
             while (m) msg[k++] = t[--m]; }
      msg[k++] = 10; msg[k] = 0; klog(msg); }

    /* ------------------------------------------------------- main loops -- */

    u32 total_frames = 0;
    int active = 0;
    int has_web = (web_fd >= 0 && poll && accept);
    int input_src = 0;          /* 0 undecided, 1 DualSense, 2 web page */
    u16 web_pad = 0;
    s32 web_client = -1;

    for (;;) {
        int selected = 0;

        /* No ROMs on disk yet: wait here instead of quitting, pumping the
         * web server so the FIRST rom can arrive over HTTP (POST /rom via
         * :9030). The upload endpoint used to run only inside the picker,
         * which needs a ROM to exist -- a bootstrap deadlock that made the
         * very first upload impossible. Rescans the ROM dirs about once a
         * second; drops into the picker as soon as anything appears.
         * R1-hold quits. */
        if (rom_count == 0) {
            u16 wb0 = 0;
            klog("No ROMs: waiting for an upload on :9030 (R1 quits)\n");
            for (int f = 0; ; f++) {
                ui_fill(ui_screen, M_COL_APPBG);
                menu_panel(ui_screen, 90, 70, 300, 130);
                draw_centered_scale(ui_screen, 240, 84, "GBAC0RE", accent_colors[g_accent_idx], 2);
                if (roms_expected > 0) {
                    /* Batch upload: show progress bar. */
                    draw_centered_scale(ui_screen, 240, 106, "RECEIVING ROMS",
                                        M_COL_WARN, 1);
                    ui_rect_alpha(ui_screen, 110, 122, 260, 8, M_COL_BORDER);
                    int fw = 258 * roms_received / roms_expected;
                    if (fw > 258) fw = 258;
                    if (fw > 0)
                        ui_rect_alpha(ui_screen, 111, 123, fw, 6,
                                      accent_colors[g_accent_idx]);
                } else {
                    draw_centered_scale(ui_screen, 240, 106, "NO ROMS FOUND", M_COL_WARN, 1);
                    ui_rect_alpha(ui_screen, 106, 120, 268, 1, M_COL_BORDER);
                    draw_centered_scale(ui_screen, 240, 128, "UPLOAD .GBA FILES TO", M_COL_SUB, 1);
                    draw_centered_scale(ui_screen, 240, 140, "HTTP://<PS5 IP>:9030", M_COL_SUB, 1);
                }
                if (has_web) {
                    web_handle(G, poll, accept, recvfrom, sendto, kclose,
                               setsockopt_fn, web_fd, &web_client,
                               web_page, web_len,
                               kopen, kwrite, kmkdir, kunlink, mmap, &wb0);
                }
                bridge_poll();  /* keep a hosted link session alive while waiting for ROMs */
                /* A finished upload lands in /temp0/roms/; rescan so the
                 * picker appears without a relaunch. In batch mode
                 * (roms_expected > 0) wait until all announced ROMs arrive;
                 * otherwise open the picker as soon as anything appears. */
                if ((f % 60) == 59 && (s64)roms != -1) {
                    int n = scan_all_dirs(G, kopen, kclose, getdents,
                                          mmap, munmap, roms, 0);
                    int batch_done = (roms_expected > 0 &&
                                      roms_received >= roms_expected);
                    int single_hit = (roms_expected == 0 && n > 0);
                    if (n > 0 && (batch_done || single_hit)) {
                        rom_count = n;
                        rom_title_post_scan(G, kopen, kread, kclose,
                                            roms, rom_count);
                        klog("ROMs arrived, opening picker\n");
                        roms_expected = 0;  /* reset for next boot */
                        roms_received = 0;
                        break;
                    }
                }
                blit_ui((u32 *)fbs[active], ui_screen);
                NC(G, vid_flip, (u64)video, (u64)active, 1, (u64)f, 0, 0);
                if (eq && wait_eq) {
                    u8 evt[64]; s32 cnt = 0;
                    NC(G, wait_eq, eq, (u64)evt, 1, (u64)&cnt, 0, 0);
                }
                active ^= 1;
            }
        }
            /* ROMs are ready: show the pause menu as the startup menu.
               rom_title=NULL shows "NO ROM LOADED" instead of "RESUME".
               LOAD ROM goes to the picker; QUIT exits. */
            {
                struct pause_ctx sc = {
                    .G = G,
                    .pad_read = pad_read, .pad_h = pad_h, .pad_buf = pad_buf,
                    .has_web = 0,
                    .web_handle = 0, .poll = 0, .accept = 0,
                    .recvfrom = 0, .sendto = 0, .kclose = 0,
                    .setsockopt_fn = 0,
                    .web_fd = -1, .web_client = 0,
                    .web_page = 0, .web_len = 0,
                    .kopen = kopen, .kwrite = kwrite, .kmkdir = kmkdir,
                    .kunlink = kunlink, .mmap = mmap,
                    .fbs = fbs, .active = &active,
                    .video = video, .vid_flip = vid_flip,
                    .eq = eq, .wait_eq = wait_eq, .usleep = usleep,
                    .rom_title = 0,
                    .web_pad = 0, .input_src = 0,
                    .total_frames = &total_frames,
                };
                g_menu_level = 0;
                int sact = run_pause_menu(&sc);
                if (sact == MENU_QUIT) {
                    klog("startup menu: quit\n");
                    goto done;
                }
                /* MENU_ROMS (LOAD ROM) or MENU_RESUME (no-op with no ROM):
                   fall through to the picker. */
                klog("startup menu: to picker\n");
            }

        /* ---------------------------------------------------- picker --- */
        {
            int cursor = 0, scroll = 0, hold = 0;
            /* Start as if every button were already held, so a button still
               down on the way back from a game cannot read as a fresh press.
               Starting at 0 meant a held Cross re-selected the same ROM
               immediately, over and over, on returning to the picker. */
            u16 prev_btn = 0xFFFF;
            /* Last successfully-polled native buttons. A failed poll reuses
               this WITHOUT touching prev_btn: writing a fake 0 into prev_btn
               would arm a spurious fresh-press on the next good read of a
               held button (this auto-selected the top ROM when X was held
               coming back from the pause menu). */
            u16 last_btn = 0;
            /* Nine 1x rows at a 12px step inside the panel: the list spans
               y=88..192, with "..." overflow markers at y=79/196, a divider
               at y=210 and the footer at y=218. */
            int visible = 9;
            if (visible > rom_count) visible = rom_count;

            for (;;) {
                u16 btn = 0;
                u16 wb = 0; int web_got = 0;
                bridge_poll();  /* keep a hosted link session alive here too */

                if (has_web) {
                    web_got = web_handle(G, poll, accept, recvfrom, sendto, kclose,
                                         setsockopt_fn, web_fd, &web_client,
                                         web_page, web_len,
                                         kopen, kwrite, kmkdir, kunlink, mmap,
                                         &wb);
                    if (web_got) web_pad = wb;
                }

                s32 nb = read_native_pad(G, pad_read, pad_h, pad_buf);

                /* The first real button press decides which source owns the
                   session; mixing the two mid-game reads as stuck input.
                   A failed poll is "no new information": reuse the last
                   good buttons and leave prev_btn untouched, so a held
                   button can never read as a fresh press. */
                int poll_ok = (nb >= 0);
                if (!poll_ok && input_src != 2) {
                    btn = last_btn;
                } else if (input_src == 0) {
                    btn = 0;
                    if (nb > 0 && nb < GBA_CMD_MENU) {
                        input_src = 1; btn = (u16)nb;
                        klog("Input: native pad\n");
                    } else if (web_got && web_pad > 0 && web_pad < GBA_CMD_MENU) {
                        input_src = 2; btn = web_pad;
                        klog("Input: web controller\n");
                    }
                    if (web_got && web_pad >= GBA_CMD_MENU) btn = web_pad;
                    if (nb >= GBA_CMD_MENU) btn = (u16)nb;
                    last_btn = btn;
                } else if (input_src == 1) {
                    btn = (u16)nb;
                    last_btn = btn;
                } else {
                    btn = web_pad;
                }

                if (btn >= GBA_CMD_MENU) web_pad = 0;

                /* GBA_CMD_MENU is the "back to picker" command, not a button
                 * mask, and its bits overlap the real ones. Letting it through
                 * meant a press of L1 in the picker read as game input, and
                 * since L1 in a game returns to the picker, the two bounced
                 * off each other and reloaded the cartridge. L1 has no meaning
                 * here: it is the way back FROM a game. */
                if (btn >= GBA_CMD_MENU) btn = 0;

                u16 pressed = btn & ~prev_btn;
                int move = 0;
                if (btn & GBA_BTN_UP) {
                    hold++;
                    if ((pressed & GBA_BTN_UP) || (hold > 12 && hold % 4 == 0)) move = -1;
                } else if (btn & GBA_BTN_DOWN) {
                    hold++;
                    if ((pressed & GBA_BTN_DOWN) || (hold > 12 && hold % 4 == 0)) move = 1;
                } else {
                    hold = 0;
                }

                if (move) {
                    cursor += move;
                    if (cursor < 0) cursor = rom_count - 1;
                    if (cursor >= rom_count) cursor = 0;
                    if (cursor < scroll) scroll = cursor;
                    if (cursor >= scroll + visible) scroll = cursor - visible + 1;
                }
                if ((pressed & GBA_BTN_A) || (pressed & GBA_BTN_START)) { selected = cursor; break; }
                /* Only a real poll moves the edge detector: a failed poll
                   reuses last_btn but must not rewrite prev_btn. */
                if (poll_ok || input_src == 2) prev_btn = btn;

                ui_fill(ui_screen, M_COL_APPBG);
                menu_panel(ui_screen, 70, 28, 340, 214);

                draw_centered_scale(ui_screen, 240, 42, "GBAC0RE", accent_colors[g_accent_idx], 2);
                draw_centered_scale(ui_screen, 240, 62, "SELECT A GAME", M_COL_SUB, 1);
                ui_rect_alpha(ui_screen, 86, 76, 308, 1, M_COL_BORDER);

                if (scroll > 0)
                    draw_centered_scale(ui_screen, 240, 79, "...", M_COL_HINT, 1);

                int ly = 88;
                for (int i = 0; i < visible && scroll + i < rom_count; i++) {
                    int idx = scroll + i;
                    int iy  = ly + i * 12;
                    int sel = (idx == cursor);
                    if (sel)
                        ui_rrect(ui_screen, 82, iy - 3, 316, 14, 5, 0xD69BBC0Fu);
                    draw_str_scale(ui_screen, 96, iy, roms[idx].display,
                                   sel ? M_COL_PILLTXT : M_COL_TEXT, 1);
                }

                if (scroll + visible < rom_count)
                    draw_centered_scale(ui_screen, 240, 196, "...", M_COL_HINT, 1);

                ui_rect_alpha(ui_screen, 86, 210, 308, 1, M_COL_BORDER);
                draw_centered_scale(ui_screen, 240, 218,
                                    "UP DOWN BROWSE   CROSS PLAY",
                                    M_COL_HINT, 1);

                blit_ui((u32 *)fbs[active], ui_screen);
                NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);
                if (eq && wait_eq) {
                    u8 evt[64]; s32 cnt = 0;
                    NC(G, wait_eq, eq, (u64)evt, 1, (u64)&cnt, 0, 0);
                }
                active ^= 1;
                total_frames++;
            }
        }

        /* ------------------------------------------------------ load --- */

        const char *rom_path = roms[selected].filename;

        clear_fb((u32 *)fbs[0]);
        clear_fb((u32 *)fbs[1]);

        if (!gba_load_rom(rom_path)) {
            for (int f = 0; f < 120; f++) {
                ui_fill(ui_screen, M_COL_APPBG);
                menu_panel(ui_screen, 90, 95, 300, 80);
                draw_centered_scale(ui_screen, 240, 109, "LOAD FAILED", M_COL_WARN, 2);
                draw_centered_scale(ui_screen, 240, 135, roms[selected].display, M_COL_SUB, 1);
                blit_ui((u32 *)fbs[active], ui_screen);
                NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);
                if (eq && wait_eq) { u8 e[64]; s32 c = 0; NC(G, wait_eq, eq, (u64)e, 1, (u64)&c, 0, 0); }
                active ^= 1;
                total_frames++;
            }
            continue;
        }

        klog(gba_title());
        klog("\n");

        /* Briefly show the proper ROM name (from the internal header,
           formatted) before starting the game. */
        {
            char pretty[32];
            format_rom_name(roms[selected].display, pretty, sizeof pretty);
            for (int f = 0; f < 120; f++) {
                ui_fill(ui_screen, M_COL_APPBG);
                draw_centered_scale(ui_screen, 240, 120, pretty,
                                    accent_colors[g_accent_idx], 2);
                blit_ui((u32 *)fbs[active], ui_screen);
                NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);
                if (eq && wait_eq) { u8 e[64]; s32 c = 0; NC(G, wait_eq, eq, (u64)e, 1, (u64)&c, 0, 0); }
                active ^= 1;
                total_frames++;
            }
            clear_fb((u32 *)fbs[0]);
            clear_fb((u32 *)fbs[1]);
        }

        /* ------------------------------------------------------ play --- */

        int back_to_menu = 0;
        u16 cur_pad = 0;
        u16 prev_cmd = 0;       /* edge-detect the L1 menu toggle */
        int prev_r1 = 0;        /* edge-detect the R1 FF toggle */
        int ff_level = 0;       /* 0=off, 1=2x, 2=4x */
        
        int trace = 3;          /* phase-trace the opening frames, then quieten */

        for (;;) {
            u16 wb = 0; int web_got = 0;
            bridge_poll();  /* link-cable pump: joiner answers here each frame */
            if (has_web) {
                web_got = web_handle(G, poll, accept, recvfrom, sendto, kclose,
                                     setsockopt_fn, web_fd, &web_client,
                                     web_page, web_len,
                                     kopen, kwrite, kmkdir, kunlink, mmap,
                                     &wb);
                if (web_got) web_pad = wb;
            }

            s32 nb = read_native_pad(G, pad_read, pad_h, pad_buf);

            if (input_src == 0) {
                if (nb > 0 && nb < GBA_CMD_MENU) { input_src = 1; cur_pad = (u16)nb; klog("Input: native pad\n"); }
                else if (web_got && web_pad > 0 && web_pad < GBA_CMD_MENU) { input_src = 2; cur_pad = web_pad; klog("Input: web controller\n"); }
                if (web_got && web_pad >= GBA_CMD_MENU) cur_pad = web_pad;
                if (nb >= GBA_CMD_MENU) cur_pad = (u16)nb;
            } else if (input_src == 1) {
                if (nb >= 0) cur_pad = (u16)nb;
            } else {
                cur_pad = web_pad;
            }

            if (cur_pad >= GBA_CMD_MENU) web_pad = 0;

            /* R1 toggles fast-forward: OFF -> 2x -> 4x -> OFF. Edge-triggered on
               the raw R1 bit (0x800). The GBA buttons held with R1 still
               register (R1 does not overwrite the button word in ds_to_gba). */
            int r1_now = (last_pad_raw & 0x800) != 0;
            if (r1_now && !prev_r1) {
                ff_level = (ff_level + 1) % 3;
            }
            prev_r1 = r1_now;
            int ff_frames = ff_level == 0 ? 1 : (ff_level == 1 ? 2 : 4);

            /* L1 opens the pause menu WITHOUT resetting the game: the core
               stays loaded, gba_run_frame() is just not called while the
               menu is up. "CHANGE ROM" inside the menu is the old L1
               behaviour (save + back to picker). Edge-triggered, and the
               menu waits for release before returning, so one press can
               never toggle it open-shut-open. */
            if (cur_pad == GBA_CMD_MENU && prev_cmd != GBA_CMD_MENU) {
                klog("game: opening pause menu\n");
                struct pause_ctx pc = {
                    .G = G,
                    .pad_read = pad_read, .pad_h = pad_h, .pad_buf = pad_buf,
                    .has_web = has_web,
                    .web_handle = web_handle, .poll = poll, .accept = accept,
                    .recvfrom = recvfrom, .sendto = sendto, .kclose = kclose,
                    .setsockopt_fn = setsockopt_fn,
                    .web_fd = web_fd, .web_client = &web_client,
                    .web_page = web_page, .web_len = web_len,
                    .kopen = kopen, .kwrite = kwrite, .kmkdir = kmkdir,
                    .kunlink = kunlink, .mmap = mmap,
                    .fbs = fbs, .active = &active,
                    .video = video, .vid_flip = vid_flip,
                    .eq = eq, .wait_eq = wait_eq, .usleep = usleep,
                    .rom_title = gba_title(),
                    .web_pad = &web_pad, .input_src = &input_src,
                    .total_frames = &total_frames,
                };
                int act;
                g_menu_level = 0;  /* pause menu always opens at the top level */
                act = run_pause_menu(&pc);
                if (act == MENU_QUIT) {
                    klog("menu: quit\n");
                    gba_save_store();
                    goto done;
                }
                if (act == MENU_ROMS) {
                    klog("menu: back to picker\n");
                    gba_save_store();
                    back_to_menu = 1; cur_pad = 0; break;
                }
                klog("menu: resumed\n");
                /* The menu flips between both framebuffers on redraw; in 3:2
                   mode blit_scale leaves the outer bars untouched, so a stale
                   menu in the inactive buffer would flicker behind the game.
                   Clear both (v1.2.10: 16:9->3:2 toggle flicker). */
                clear_fb((u32 *)fbs[0]);
                clear_fb((u32 *)fbs[1]);
                cur_pad = 0;
            }
            prev_cmd = cur_pad;

            gba_set_input(cur_pad);

            /* The first few frames are traced phase by phase. Everything here
               runs for the first time only once a real cartridge is loaded, so
               a fault has no other way of telling us which stage it died in --
               the log otherwise just stops after the ROM loads. */
            if (trace) klog("frame: input applied\n");
            gba_trace = trace;
            for (int ffi = 0; ffi < ff_frames; ffi++) {
                gba_run_frame();
            }
            gba_trace = 0;
            if (trace) klog("frame: emulation ok\n");

            /* FF indicator into the 240x160 buffer before scaling. */
            if (ff_level > 0) draw_ff_indicator(ff_level);

            if (g_fullscreen)
                blit_fullscreen((u32 *)fbs[active], gba_framebuffer);
            else
                blit_scale((u32 *)fbs[active], gba_framebuffer);
            if (trace) klog("frame: blit ok\n");

            NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);
            if (trace) klog("frame: flip submitted\n");

            /* Submitted after the flip on purpose: this call blocks until the
               audio queue drains, which is what actually paces the emulator
               at the GBA's 59.73Hz rather than the display's 60Hz. During
               fast-forward the Nx audio is decimated N:1 and played at the
               normal rate: chipmunk pitch, 1x block time, Nx speedup kept. */
            if (ff_level > 0) gba_audio_ff_flush(ff_frames);
            else gba_audio_flush();
            if (trace) klog("frame: audio ok\n");

            if (eq && wait_eq) {
                u8 evt[64]; s32 cnt = 0;
                NC(G, wait_eq, eq, (u64)evt, 1, (u64)&cnt, 0, 0);
            }
            if (trace) { klog("frame: vsync ok -- frame complete\n"); trace--; }

            active ^= 1;
            total_frames++;
            ext->frame_count = total_frames;
            if ((total_frames % 600) == 0) {
                char ab[64];
                snprintf(ab, sizeof ab, "arena: used %lu/%lu bytes\n",
                         (unsigned long)arena_used(),
                         (unsigned long)arena_size_get());
                klog(ab);
            }
        }

        gba_save_store();
        if (!back_to_menu) break;
    }

done:
    klog("Shutting down...\n");
    gba_unload();

    if (aud_close && audio_h >= 0) NC(G, aud_close, (u64)audio_h, 0,0,0,0,0);

    clear_fb((u32 *)fbs[0]);
    clear_fb((u32 *)fbs[1]);
    NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);
    if (usleep) NC(G, usleep, 50000, 0,0,0,0,0);

    if (vid_close && video >= 0) NC(G, vid_close, (u64)video, 0,0,0,0,0);
    if (delete_eq && eq)         NC(G, delete_eq, eq, 0,0,0,0,0);
    if (web_client >= 0 && kclose) NC(G, kclose, (u64)web_client, 0,0,0,0,0);
    if (web_fd >= 0 && kclose)     NC(G, kclose, (u64)web_fd, 0,0,0,0,0);

    if (munmap) {
        if ((s64)roms != -1)
            NC(G, munmap, (u64)roms, (u64)(sizeof(struct rom_entry) * MAX_ROMS), 0,0,0,0);
        NC(G, munmap, (u64)arena, arena_size, 0,0,0,0);
    }

    klog("Clean exit\n");
    ext->status = 0;
    ext->step = 99;
    ext->frame_count = total_frames;
}
