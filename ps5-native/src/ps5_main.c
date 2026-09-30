/* GbaC0re PS5 native — application main loop.
 *
 * Version: GbaC0re PS5 native v2.0.0. New lineage: native Prospero homebrew,
 * NOT a LuaC0re payload. No Lua bootstrap, no blob upload, no JIT pool,
 * no arenas — plain int main(), real libc, real BSD sockets.
 *
 * All console I/O goes through ps5_platform.h (SceVideoOut / SceAudioOut /
 * ScePad backend). This file is game logic only: ROM picker, pause menu,
 * frame loop, fast-forward, link polling, save management.
 *
 * Controls (DualSense):
 *   D-pad / left stick: d-pad      Cross: A        Circle: B
 *   Create: Select                 Options: Start
 *   L2/R2 (triggers): GBA L/R
 *   L1: pause menu (edge)          R1: fast-forward OFF->2x->4x->OFF (edge)
 *   In menus: D-pad navigate, Cross select/adjust, Circle back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>    /* struct dirent layout + getdents prototype */
#include <sys/stat.h>

#include "ps5_core.h"
#include "ps5_gba.h"
#include "ps5_platform.h"
#include "menu.h"
#include "ui.h"
#include "savestate.h"
#include "bridge.h"  /* link-cable netplay (UDP) */

/* UI draw target: 480x270 ARGB, presented via plat_video_present_ui(). */
static u32 ui_buffer[UI_W * UI_H];

/* Draws the menu, submits the flip; the caller flushes menu audio, then
 * syncs (v129 pacing: flip submitted before the blocking audio). */
static void menu_draw_submit(int cursor, const char *toast, int toast_frames) {
    memset(ui_buffer, 0, sizeof(ui_buffer));
    menu_draw(ui_buffer, ps5_gba_title(), cursor, toast, toast_frames);
    plat_video_compose_ui(ui_buffer);
    plat_video_submit();
}

/* ------------------------------------------------------------------ */
/* ROM picker                                                          */
/* ------------------------------------------------------------------ */

#define PICKER_MAX 256

static char picker_paths[PICKER_MAX][256];
static char picker_names[PICKER_MAX][64];
static int picker_count = 0;

static int has_gba_ext(const char *name) {
    size_t n = strlen(name);
    if (n < 5) return 0;
    const char *e = name + n - 4;
    return (e[0] == '.' &&
            (e[1] == 'g' || e[1] == 'G') &&
            (e[2] == 'b' || e[2] == 'B') &&
            (e[3] == 'a' || e[3] == 'A'));
}

static void scan_roms(void) {
    /* opendir/readdir/closedir are CONFIRMED BROKEN on PS5 hardware
       (ps5link catalog notes: the import never binds, SIGSEGV). Use the
       lower-level getdents syscall directly — it is in the NID catalog. */
    static char dentbuf[8192];

    picker_count = 0;
    int fd = open(PS5_ROM_DIR, O_RDONLY);
    if (fd < 0) {
        ps5_diag_logf("PS5N: no %s directory\n", PS5_ROM_DIR);
        return;
    }
    for (;;) {
        int n = getdents(fd, dentbuf, (int)sizeof(dentbuf));
        if (n <= 0) break;
        int off = 0;
        while (off < n && picker_count < PICKER_MAX) {
            struct dirent *de = (struct dirent *)(dentbuf + off);
            if (de->d_reclen == 0) break;
            if (de->d_namlen > 0 && de->d_type != DT_DIR &&
                has_gba_ext(de->d_name)) {
                snprintf(picker_paths[picker_count], sizeof(picker_paths[0]),
                         "%s%s", PS5_ROM_DIR, de->d_name);
                /* Display name: stem of the file name. */
                const char *dot = strrchr(de->d_name, '.');
                size_t len = dot ? (size_t)(dot - de->d_name)
                                 : strlen(de->d_name);
                if (len >= sizeof(picker_names[0]))
                    len = sizeof(picker_names[0]) - 1;
                memcpy(picker_names[picker_count], de->d_name, len);
                picker_names[picker_count][len] = 0;
                picker_count++;
            }
            off += de->d_reclen;
        }
        if (picker_count >= PICKER_MAX) break;
    }
    close(fd);
    /* Sort by display name, keeping paths aligned. */
    for (int i = 0; i < picker_count; i++) {
        for (int j = i + 1; j < picker_count; j++) {
            if (strcmp(picker_names[i], picker_names[j]) > 0) {
                char t[256];
                memcpy(t, picker_names[i], sizeof(t));
                memcpy(picker_names[i], picker_names[j], sizeof(t));
                memcpy(picker_names[j], t, sizeof(t));
                memcpy(t, picker_paths[i], sizeof(t));
                memcpy(picker_paths[i], picker_paths[j], sizeof(t));
                memcpy(picker_paths[j], t, sizeof(t));
            }
        }
    }
    ps5_diag_logf("PS5N: ROM scan: %d found in %s\n", picker_count, PS5_ROM_DIR);
}

/* Modal ROM picker. Returns the chosen path, or NULL on cancel. */
static const char *run_picker(void) {
    scan_roms();
    int cursor = 0;
    int prev_l1 = 0;
    u16 prev = 0;

    while (1) {
        int l1 = 0, r1 = 0;
        u16 btn = plat_pad_buttons(&l1, &r1);
        u16 pressed = btn & ~prev;
        prev = btn;

        if ((pressed & GBA_BTN_UP) && cursor > 0) {
            cursor--;
            ps5_gba_ui_click(CLICK_TICK);
        }
        if ((pressed & GBA_BTN_DOWN) && cursor < picker_count - 1) {
            cursor++;
            ps5_gba_ui_click(CLICK_TICK);
        }
        if (pressed & GBA_BTN_A) {
            ps5_gba_ui_click(CLICK_CONFIRM);
            if (picker_count > 0)
                return picker_paths[cursor];
        }
        if ((pressed & GBA_BTN_B) || (l1 && !prev_l1)) {
            ps5_gba_ui_click(CLICK_BACK);
            return NULL;  /* cancel */
        }
        prev_l1 = l1;

        /* Draw the list into the UI buffer. */
        memset(ui_buffer, 0, sizeof(ui_buffer));
        ui_rrect(ui_buffer, 60, 20, 360, 230, 10, M_COL_BORDER);
        ui_rrect(ui_buffer, 61, 21, 358, 228, 9, 0xFF141926u);
        draw_centered_scale(ui_buffer, 240, 34, "SELECT ROM",
                            accent_colors[g_accent_idx], 2);
        if (picker_count == 0) {
            draw_centered_scale(ui_buffer, 240, 120, "NO ROMS FOUND",
                                M_COL_WARN, 1);
            draw_centered_scale(ui_buffer, 240, 145, "ADD .GBA FILES TO",
                                M_COL_SUB, 1);
            draw_centered_scale(ui_buffer, 240, 162, PS5_ROM_DIR,
                                M_COL_SUB, 1);
        } else {
#define PICKER_VISIBLE 9
            int scroll = 0;
            if (cursor >= PICKER_VISIBLE) scroll = cursor - PICKER_VISIBLE + 1;
            int iy = 70;
            for (int vi = 0; vi < PICKER_VISIBLE && scroll + vi < picker_count; vi++) {
                int i = scroll + vi;
                if (i == cursor)
                    ui_rrect(ui_buffer, 74, iy - 4, 332, 22, 6,
                             accent_colors[g_accent_idx]);
                char label[40];
                snprintf(label, sizeof(label), "%.36s", picker_names[i]);
                draw_str_scale(ui_buffer, 88, iy, label,
                               i == cursor ? M_COL_PILLTXT : M_COL_TEXT, 1);
                iy += 24;
                if (iy > 218) break;
            }
#undef PICKER_VISIBLE
        }
        draw_centered_scale(ui_buffer, 240, 232, "CROSS LOAD   CIRCLE BACK",
                            M_COL_HINT, 1);
        plat_video_compose_ui(ui_buffer);
        plat_video_submit();

        ps5_gba_audio_flush_menu();
        plat_video_sync();
        bridge_poll();
        plat_delay_ms(16);
    }
}

/* ------------------------------------------------------------------ */
/* Pause menu                                                          */
/* ------------------------------------------------------------------ */

/* Item indices (must match menu.c MENU_NITEMS layout). */
#define M_RESUME 0
#define M_SAVE   1
#define M_LOAD   2
#define M_CHANGE 3
#define M_OPTS   4
#define M_LINK   5
#define M_QUIT   6  /* always last */

static char menu_toast[64] = "";
static int menu_toast_frames = 0;

static void menu_set_toast(const char *msg) {
    snprintf(menu_toast, sizeof(menu_toast), "%s", msg);
    menu_toast_frames = 120;
}

/* Link host toggle (v2.0.0: PS5 is the host; join is future work). */
static void link_toggle(int rom_loaded) {
    if (bridge_role() == LINK_ROLE_NONE) {
        if (bridge_host(0)) {
            if (rom_loaded) ps5_gba_link_attach();
            ps5_diag_log("PS5N: link hosting (waiting for joiner)\n");
            menu_set_toast("LINK: HOSTING");
        } else {
            ps5_diag_log("PS5N: link host failed (port in use?)\n");
            menu_set_toast("HOST FAILED");
        }
    } else {
        bridge_disconnect();
        ps5_diag_log("PS5N: link disconnected\n");
        menu_set_toast("LINK: OFF");
    }
}

/* Returns: 0 = resume, 1 = quit app, 2 = change ROM. */
static int run_menu(void) {
    int cursor = 0;
    u16 prev = 0;
    int prev_l1 = 0;

    ps5_gba_ui_click(CLICK_CONFIRM);

    while (1) {
        int l1 = 0, r1 = 0;
        u16 btn = plat_pad_buttons(&l1, &r1);
        u16 pressed = btn & ~prev;
        prev = btn;

        if (pressed & GBA_BTN_UP) {
            int n = (g_menu_level == 0) ? MENU_NITEMS : OPT_NITEMS;
            cursor = (cursor + n - 1) % n;
            ps5_gba_ui_click(CLICK_TICK);
        }
        if (pressed & GBA_BTN_DOWN) {
            int n = (g_menu_level == 0) ? MENU_NITEMS : OPT_NITEMS;
            cursor = (cursor + 1) % n;
            ps5_gba_ui_click(CLICK_TICK);
        }

        /* Left/right adjust the OPTIONS live settings. */
        if (g_menu_level == 1 && (pressed & (GBA_BTN_LEFT | GBA_BTN_RIGHT))) {
            int dir = (pressed & GBA_BTN_RIGHT) ? 1 : -1;
            ps5_gba_ui_click(CLICK_TICK);
            if (cursor == 0) {
                g_fullscreen = !g_fullscreen;
            } else if (cursor == 1) {
                g_accent_idx = (g_accent_idx + dir + ACCENT_NCOLORS) % ACCENT_NCOLORS;
            } else if (cursor == 2) {
                g_volume += dir;
                if (g_volume < 0) g_volume = 0;
                if (g_volume > 10) g_volume = 10;
            }
        }

        int action = -1;  /* -1 none, 0 resume, 1 quit, 2 change ROM */
        if (pressed & GBA_BTN_A) {
            ps5_gba_ui_click(CLICK_CONFIRM);
            if (g_menu_level == 0) {
                switch (cursor) {
                case M_RESUME: action = 0; break;
                case M_SAVE:
                    menu_set_toast(gba_state_save() == 0 ? "STATE SAVED" : "SAVE FAILED");
                    break;
                case M_LOAD:
                    menu_set_toast(gba_state_load() == 0 ? "STATE LOADED" : "NO STATE");
                    break;
                case M_CHANGE: action = 2; break;
                case M_OPTS:
                    g_menu_level = 1; cursor = 0;
                    break;
                case M_LINK:
                    link_toggle(ps5_gba_is_loaded());
                    break;
                case M_QUIT: action = 1; break;
                }
            } else {
                /* OPTIONS level. */
                switch (cursor) {
                case 0: g_fullscreen = !g_fullscreen; break;
                case 1: g_accent_idx = (g_accent_idx + 1) % ACCENT_NCOLORS; break;
                case 2:
                    g_volume = (g_volume + 1) % 11;
                    break;
                case 3:
                    ps5_gba_save_store();
                    menu_set_toast("GAME SAVED");
                    break;
                case 4:
                    g_menu_level = 0; cursor = M_OPTS;
                    break;
                }
            }
        }
        if ((pressed & GBA_BTN_B) || (l1 && !prev_l1)) {
            ps5_gba_ui_click(CLICK_BACK);
            if (g_menu_level == 1) {
                g_menu_level = 0; cursor = M_OPTS;
            } else {
                action = 0;  /* resume */
            }
        }
        prev_l1 = l1;

        if (menu_toast_frames > 0) menu_toast_frames--;
        menu_draw_submit(cursor, menu_toast, menu_toast_frames);
        ps5_gba_audio_flush_menu();
        plat_video_sync();
        bridge_poll();

        if (action >= 0) {
            if (action != 2) ps5_gba_ui_click(CLICK_BACK);
            g_menu_level = 0;
            return action;
        }
        plat_delay_ms(16);
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void ensure_dirs(void) {
    mkdir(PS5_SAVE_DIR, 0755);
    mkdir(PS5_STATE_DIR, 0755);
    mkdir(PS5_ROM_DIR, 0755);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    ps5_diag_notify("GbaC0re v2.0.0 boot");
    ps5_diag_logf("PS5N: %s booting\n", VERSION_STR);

    if (!plat_video_init()) {
        ps5_diag_log("PS5N: FATAL: video init failed\n");
        return 1;
    }
    ps5_diag_log("PS5N: video OK\n");

    if (!plat_audio_init()) {
        ps5_diag_log("PS5N: FATAL: audio init failed\n");
        return 1;
    }
    ps5_diag_log("PS5N: audio OK\n");
    ps5_gba_set_audio(plat_audio_submit);

    if (!plat_pad_init())
        ps5_diag_log("PS5N: WARNING: no game controller found\n");

    ensure_dirs();

    /* Boot: exactly one ROM -> load it; otherwise the picker. */
    scan_roms();
    int rom_loaded = 0;
    if (picker_count == 1) {
        ps5_diag_logf("PS5N: single ROM, loading %s\n", picker_paths[0]);
        rom_loaded = ps5_gba_load_rom(picker_paths[0]);
    } else {
        const char *pick = run_picker();
        if (pick) rom_loaded = ps5_gba_load_rom(pick);
    }
    if (rom_loaded) {
        ps5_diag_notify("ROM loaded");
        ps5_diag_logf("PS5N: ROM loaded: %s\n", ps5_gba_title());
    } else {
        ps5_diag_log("PS5N: no ROM loaded at boot\n");
    }

    int running = 1;
    int ff_mode = 0;  /* 0=off, 1=2x, 2=4x */
    int prev_l1 = 0, prev_r1 = 0;
    int first_frame = 1;

    while (running) {
        int l1 = 0, r1 = 0;
        u16 btn_all = plat_pad_buttons(&l1, &r1);

        /* Edge-triggered commands. */
        if (l1 && !prev_l1 && rom_loaded) {
            int res = run_menu();
            if (res == 1) {
                running = 0;
            } else if (res == 2) {
                /* CHANGE ROM: save, then pick. Cancel keeps the current ROM. */
                ps5_gba_save_store();
                const char *pick = run_picker();
                if (pick) {
                    ps5_gba_unload();
                    if (ps5_gba_load_rom(pick)) {
                        rom_loaded = 1;
                        ff_mode = 0;
                        ps5_diag_logf("PS5N: ROM loaded: %s\n", ps5_gba_title());
                    } else {
                        ps5_diag_logf("PS5N: ROM load failed: %s\n", pick);
                        rom_loaded = 0;
                    }
                }
            }
            /* Drain L1 so the menu doesn't reopen instantly. */
            while (1) {
                int rl1 = 0;
                plat_pad_buttons(&rl1, NULL);
                if (!rl1) break;
                plat_delay_ms(10);
            }
        }
        if (r1 && !prev_r1 && rom_loaded) {
            ff_mode = (ff_mode + 1) % 3;
            ps5_diag_logf("PS5N: fast-forward %s\n",
                          ff_mode == 0 ? "OFF" : (ff_mode == 1 ? "2x" : "4x"));
        }
        prev_l1 = l1; prev_r1 = r1;

        bridge_poll();

        u16 btn = 0;
        if (rom_loaded) btn = btn_all;
        ps5_gba_set_input(btn);

        if (rom_loaded) {
            int frames = (ff_mode == 0) ? 1 : (ff_mode == 1 ? 2 : 4);
            for (int i = 0; i < frames; i++)
                ps5_gba_run_frame();
            /* v129 pacing: flip submitted BEFORE the blocking audio flush;
               the audio drain is the master clock and overlaps the vsync
               wait instead of adding to it. */
            plat_video_compose_game();
            plat_video_submit();
            if (ff_mode == 0)
                ps5_gba_audio_flush();
            else
                ps5_gba_audio_flush_chipmunk(ff_mode == 1 ? 2 : 4);
            plat_video_sync();
            if (first_frame) {
                first_frame = 0;
                ps5_diag_log("PS5N: FIRST FRAME PRESENTED\n");
                ps5_diag_notify("GbaC0re running");
            }
        } else {
            /* No ROM: dark screen with prompt. */
            memset(ui_buffer, 0, sizeof(ui_buffer));
            ui_fill(ui_buffer, 0xFF0F1410u);
            draw_centered_scale(ui_buffer, 240, 120, "GBAC0RE",
                                accent_colors[g_accent_idx], 2);
            draw_centered_scale(ui_buffer, 240, 150, "NO ROM LOADED",
                                M_COL_SUB, 1);
            draw_centered_scale(ui_buffer, 240, 175, "ADD .GBA FILES TO roms/",
                                M_COL_HINT, 1);
            plat_video_compose_ui(ui_buffer);
            plat_video_submit();
            plat_video_sync();
            plat_delay_ms(100);
        }

        /* Audio is the master clock: throttle to keep the output queue near
           ~3 frames, same pacing as the PC port. */
        if (ff_mode == 0) {
            const u32 target = (48000 / 60) * 3 * 4;
            int guard = 0;
            while (plat_audio_queued() > target && guard < 200) {
                plat_delay_ms(1);
                guard++;
            }
        }
    }

    ps5_diag_log("PS5N: shutting down\n");
    ps5_gba_save_store();
    ps5_gba_unload();
    bridge_disconnect();
    plat_pad_shutdown();
    plat_audio_shutdown();
    plat_video_shutdown();
    return 0;
}
