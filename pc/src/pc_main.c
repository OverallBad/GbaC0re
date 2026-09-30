/* GbaC0re PC — SDL2 frontend.
 *
 * Usage: ./gbac0re_pc <rom.gba>
 *
 * Controls (keyboard):
 *   Arrows/D-pad, Z=B, X=A, Enter=Start, Shift=Select, A=L, S=R
 *   ESC or P: pause menu
 *   F1: save state, F2: load state
 *   F3: fast-forward toggle (2x/4x/off)
 *
 * Gamepad (SDL): standard mapping, Start+Select opens menu.
 */

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_core.h"
#include "pc_gba.h"
#include "menu.h"
#include "ui.h"
#include "savestate.h"
#include "pc_menu_bar.h"
#include "bridge.h"  /* link-cable netplay (PC<->PS5) */

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
/* Native Windows file dialog for ROM selection. Returns 1 on success. */
static int open_rom_dialog(char *out_path, int out_size) {
    OPENFILENAMEA ofn;
    char szFile[1024] = { 0 };
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "GBA ROMs (*.gba)\0*.gba\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) {
        strncpy(out_path, szFile, out_size - 1);
        out_path[out_size - 1] = '\0';
        return 1;
    }
    return 0;
}
#else
/* Linux/macOS: try zenity, else fail gracefully. */
static int open_rom_dialog(char *out_path, int out_size) {
    FILE *fp = popen("zenity --file-selection --file-filter='GBA ROMs | *.gba' 2>/dev/null", "r");
    if (!fp) return 0;
    if (!fgets(out_path, out_size, fp)) { pclose(fp); return 0; }
    pclose(fp);
    /* Strip newline */
    out_path[strcspn(out_path, "\r\n")] = 0;
    return out_path[0] != 0;
}
#endif

/* ------------------------------------------------------------------ */
/* Video                                                               */
/* ------------------------------------------------------------------ */

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *game_tex = NULL;   /* 240x160, scaled by SDL */
static SDL_Texture *ui_tex = NULL;     /* 480x270 menu */

static int init_video(void) {
    window = SDL_CreateWindow("GbaC0re PC",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              960, 540 + MENU_BAR_H,  /* 16:9 client + menu bar */
                              SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 0;
    }
    renderer = SDL_CreateRenderer(window, -1,
                                  SDL_RENDERER_ACCELERATED |
                                  SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 0;
    }
    game_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING,
                                 GBA_W, GBA_H);
    ui_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                               SDL_TEXTUREACCESS_STREAMING,
                               UI_W, UI_H);
    if (!game_tex || !ui_tex) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        return 0;
    }
    return 1;
}

static void present_game(void) {
    SDL_UpdateTexture(game_tex, NULL, pc_gba_framebuffer, GBA_W * 4);
    /* Explicit black clear: RenderClear uses the last-set draw color, which
       would otherwise leak the menu bar's grey into the background. */
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    /* Leave room for menu bar at top: offset game down by MENU_BAR_H. */
    SDL_Rect dst = { 0, MENU_BAR_H, 0, 0 };
    int ww, wh;
    SDL_GetWindowSize(window, &ww, &wh);
    int avail_h = wh - MENU_BAR_H;
    dst.w = ww;
    dst.h = avail_h;
    if (g_fullscreen) {
        /* 16:9: stretch to fill the client area (same as PS5 default). */
    } else {
        /* 3:2: integer scale, centered on black. */
        int scale = ww / GBA_W;
        int sh = GBA_H * scale;
        if (sh > avail_h) { scale = avail_h / GBA_H; sh = GBA_H * scale; }
        if (scale < 1) scale = 1;
        dst.w = GBA_W * scale;
        dst.h = sh;
        dst.x = (ww - dst.w) / 2;
        dst.y = MENU_BAR_H + (avail_h - dst.h) / 2;
    }
    SDL_RenderCopy(renderer, game_tex, NULL, &dst);
    /* Menu bar on top */
    pc_menu_bar_render();
    SDL_RenderPresent(renderer);
}

static u32 ui_buffer[UI_W * UI_H];

static void present_menu(int cursor, const char *toast, int toast_frames) {
    /* Clear to transparent black, draw the menu. */
    memset(ui_buffer, 0, sizeof(ui_buffer));
    menu_draw(ui_buffer, pc_gba_title(), cursor, toast, toast_frames);
    SDL_UpdateTexture(ui_tex, NULL, ui_buffer, UI_W * 4);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, ui_tex, NULL, NULL);
    SDL_RenderPresent(renderer);
}

/* ------------------------------------------------------------------ */
/* Audio                                                               */
/* ------------------------------------------------------------------ */

static SDL_AudioDeviceID audio_dev = 0;

static void audio_submit_sdl(const s16 *samples, int frames) {
    /* samples: stereo s16, frames * 4 bytes. */
    SDL_QueueAudio(audio_dev, (void *)samples, frames * 4);
}

static int init_audio(void) {
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = SAMPLES_PER_BUF * 2;

    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!audio_dev) {
        fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return 0;
    }
    printf("PC: audio %dHz, %d channels\n", have.freq, have.channels);
    SDL_PauseAudioDevice(audio_dev, 0);
    pc_gba_set_audio(audio_submit_sdl);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static SDL_GameController *pad = NULL;

/* Keyboard state -> GBA buttons. */
static u16 keyboard_buttons(void) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    u16 b = 0;
    if (k[SDL_SCANCODE_X])     b |= GBA_BTN_A;
    if (k[SDL_SCANCODE_Z])     b |= GBA_BTN_B;
    if (k[SDL_SCANCODE_RETURN]) b |= GBA_BTN_START;
    if (k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_LSHIFT]) b |= GBA_BTN_SELECT;
    if (k[SDL_SCANCODE_RIGHT]) b |= GBA_BTN_RIGHT;
    if (k[SDL_SCANCODE_LEFT])  b |= GBA_BTN_LEFT;
    if (k[SDL_SCANCODE_UP])    b |= GBA_BTN_UP;
    if (k[SDL_SCANCODE_DOWN])  b |= GBA_BTN_DOWN;
    if (k[SDL_SCANCODE_S])     b |= GBA_BTN_R;
    if (k[SDL_SCANCODE_A])     b |= GBA_BTN_L;
    return b;
}

/* Gamepad -> GBA buttons. */
static u16 pad_buttons(void) {
    if (!pad) return 0;
    u16 b = 0;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) b |= GBA_BTN_A;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) b |= GBA_BTN_B;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) b |= GBA_BTN_START;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK)) b |= GBA_BTN_SELECT;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= GBA_BTN_RIGHT;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= GBA_BTN_LEFT;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= GBA_BTN_UP;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= GBA_BTN_DOWN;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b |= GBA_BTN_R;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) b |= GBA_BTN_L;
    /* Analog stick as d-pad. */
    int ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    int ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (ax > 8000) b |= GBA_BTN_RIGHT;
    if (ax < -8000) b |= GBA_BTN_LEFT;
    if (ay > 8000) b |= GBA_BTN_DOWN;
    if (ay < -8000) b |= GBA_BTN_UP;
    return b;
}

static void init_pad(void) {
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            pad = SDL_GameControllerOpen(i);
            if (pad) {
                printf("PC: gamepad: %s\n", SDL_GameControllerName(pad));
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pause menu (reuses menu_draw from the PS5 build)                     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Link-cable netplay (PC <-> PS5 over UDP).                            */
/* ------------------------------------------------------------------ */

/* Attach the bridge driver if a link session is active and a ROM is loaded. */
static void pc_link_attach_if_active(int rom_loaded) {
    if (rom_loaded && bridge_role() != LINK_ROLE_NONE)
        pc_gba_link_attach();
}

/* Modal IP-entry dialog for joining a host. Returns 1 if the user confirmed. */
static int link_join_dialog(SDL_Renderer *renderer, SDL_Window *window,
                            char *out_ip, size_t out_n) {
    char ip[64] = "192.168.0.140";  /* default: Ty's PS5 */
    int done = 0, ok = 0, fresh = 1;  /* fresh: first keystroke replaces default */
    SDL_StartTextInput();
    while (!done) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { done = 1; break; }
            if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { done = 1; ok = 1; }
                else if (k == SDLK_ESCAPE) done = 1;
                else if (k == SDLK_BACKSPACE) {
                    if (fresh) { ip[0] = 0; fresh = 0; }
                    else {
                        size_t l = strlen(ip);
                        if (l) ip[l - 1] = 0;
                    }
                }
            } else if (ev.type == SDL_TEXTINPUT) {
                for (const char *p = ev.text.text; *p; p++) {
                    if ((*p >= '0' && *p <= '9') || *p == '.') {
                        if (fresh) { ip[0] = 0; fresh = 0; }
                        size_t l = strlen(ip);
                        if (l + 1 < sizeof ip) { ip[l] = *p; ip[l + 1] = 0; }
                    }
                }
            }
        }
        bridge_poll();  /* keep an existing session alive while typing */
        int ww, wh;
        SDL_GetWindowSize(window, &ww, &wh);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        {
            int bw = 480, bh = 150;
            int bx = (ww - bw) / 2, by = (wh - bh) / 2;
            SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
            SDL_Rect box = { bx, by, bw, bh };
            SDL_RenderFillRect(renderer, &box);
            SDL_SetRenderDrawColor(renderer, 0, 221, 0, 255);
            SDL_RenderDrawRect(renderer, &box);
            const char *title = "JOIN LINK GAME";
            pc_menu_text(bx + (bw - pc_menu_text_width(title)) / 2, by + 18,
                         title, 0, 221, 0);
            pc_menu_text(bx + 30, by + 62, "HOST IP:", 220, 220, 220);
            SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
            SDL_Rect field = { bx + 30, by + 84, bw - 60, 28 };
            SDL_RenderFillRect(renderer, &field);
            SDL_SetRenderDrawColor(renderer, 120, 120, 120, 255);
            SDL_RenderDrawRect(renderer, &field);
            pc_menu_text(bx + 38, by + 90, ip, 255, 255, 255);
            const char *hint = "ENTER: JOIN   ESC: CANCEL";
            pc_menu_text(bx + (bw - pc_menu_text_width(hint)) / 2, by + 122,
                         hint, 140, 140, 140);
        }
        pc_menu_bar_render();
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
    SDL_StopTextInput();
    if (ok && ip[0]) { snprintf(out_ip, out_n, "%s", ip); return 1; }
    return 0;
}

static void link_do_host(int rom_loaded) {
    if (bridge_host(0)) {
        pc_link_attach_if_active(rom_loaded);
        printf("PC: link hosting on UDP %d (waiting for joiner)\n",
               LINK_DEFAULT_PORT);
    } else {
        printf("PC: link host failed (port in use?)\n");
    }
}

static void link_do_join(SDL_Renderer *renderer, SDL_Window *window,
                        int rom_loaded) {
    char ip[64];
    if (!link_join_dialog(renderer, window, ip, sizeof ip)) return;
    printf("PC: joining link host %s ...\n", ip);
    if (bridge_join(ip, 0)) {
        pc_link_attach_if_active(rom_loaded);
        printf("PC: link joined %s\n", ip);
    } else {
        printf("PC: link join timed out (no host at %s)\n", ip);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Join failed",
                                 "No link host answered at that IP.", window);
    }
}

static void link_do_disconnect(void) {
    /* End the UDP session but keep the SIO driver attached: re-hosting
     * or re-joining later must work without a ROM reload. */
    bridge_disconnect();
    printf("PC: link disconnected\n");
}

/* Menu item indices (must match MENU_NITEMS layout in menu.c). */
#define M_RESUME 0
#define M_SAVE   1
#define M_LOAD   2
#define M_CHANGE 3
#define M_OPTS   4
#define M_QUIT   5  /* always last */

static int menu_cursor = 0;
static char menu_toast[64] = "";
static int menu_toast_frames = 0;

static void menu_set_toast(const char *msg) {
    snprintf(menu_toast, sizeof(menu_toast), "%s", msg);
    menu_toast_frames = 120;  /* 2 seconds at 60fps */
}

/* Returns 1 to keep playing, 0 to quit the app. */
static int run_menu(void) {
    int cursor = 0;
    u16 prev = 0;

    pc_gba_ui_click(CLICK_CONFIRM);

    while (1) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) return 0;
        }

        u16 btn = keyboard_buttons() | pad_buttons();
        u16 pressed = btn & ~prev;
        prev = btn;

        if (pressed & GBA_BTN_UP) {
            cursor = (cursor + MENU_NITEMS - 1) % MENU_NITEMS;
            pc_gba_ui_click(CLICK_TICK);
        }
        if (pressed & GBA_BTN_DOWN) {
            cursor = (cursor + 1) % MENU_NITEMS;
            pc_gba_ui_click(CLICK_TICK);
        }

        int done = 0;
        int quit_app = 0;
        if (pressed & GBA_BTN_A) {
            pc_gba_ui_click(CLICK_CONFIRM);
            switch (cursor) {
            case M_RESUME:
                done = 1;
                break;
            case M_SAVE:
                if (gba_state_save() == 0)
                    menu_set_toast("STATE SAVED");
                else
                    menu_set_toast("SAVE FAILED");
                break;
            case M_LOAD:
                if (gba_state_load() == 0)
                    menu_set_toast("STATE LOADED");
                else
                    menu_set_toast("NO STATE");
                break;
            case M_CHANGE:
                /* No ROM picker on PC v1. */
                menu_set_toast("NOT YET");
                break;
            case M_OPTS:
                /* TODO: options submenu (screen/color/volume). */
                menu_set_toast("NOT YET");
                break;
            case M_QUIT:
                quit_app = 1;
                done = 1;
                break;
            }
        }
        if (pressed & GBA_BTN_B) {
            pc_gba_ui_click(CLICK_BACK);
            done = 1;  /* B = resume */
        }

        /* ESC also closes. */
        const Uint8 *k = SDL_GetKeyboardState(NULL);
        if (k[SDL_SCANCODE_ESCAPE]) {
            /* Wait for release to avoid instant reopen. */
            while (k[SDL_SCANCODE_ESCAPE]) {
                SDL_PumpEvents();
                k = SDL_GetKeyboardState(NULL);
                SDL_Delay(10);
            }
            done = 1;
        }

        if (menu_toast_frames > 0) menu_toast_frames--;
        pc_gba_audio_flush_menu();
        present_menu(cursor, menu_toast, menu_toast_frames);
        bridge_poll();  /* joiner keeps answering while paused in the menu */

        if (done) {
            pc_gba_ui_click(CLICK_BACK);
            return !quit_app;
        }

        SDL_Delay(16);  /* ~60fps menu */
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    /* ROM is optional now — without one we open to the menu bar + welcome. */
    const char *rom_arg = (argc >= 2) ? argv[1] : NULL;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    if (!init_video()) return 1;
    if (!init_audio()) return 1;
    init_pad();

    /* Menu bar (needs renderer from init_video). */
    {
        int ww, wh;
        SDL_GetWindowSize(window, &ww, &wh);
        pc_menu_bar_init(renderer, window, ww, wh);
    }

    int rom_loaded = 0;
    if (rom_arg) {
        if (!pc_gba_load_rom(rom_arg)) {
            fprintf(stderr, "Failed to load ROM: %s\n", rom_arg);
        } else {
            rom_loaded = 1;
            printf("%s — %s\n", VERSION_STR, pc_gba_title());
            pc_link_attach_if_active(rom_loaded);
        }
    }
    pc_menu_bar_set_rom_loaded(rom_loaded);
    if (!rom_loaded) {
        printf("%s — no ROM loaded (File > Open ROM)\n", VERSION_STR);
    } else {
        printf("ESC: menu | F1: save state | F2: load state | F3: fast-forward\n");
    }

    int running = 1;
    int ff_mode = 0;  /* 0=off, 1=2x, 2=4x */
    int is_fullscreen = 0;
    u16 prev_btn = 0;
    int paused = 0;
    pc_menu_bar_set_ff_mode(0);

    /* For edge-triggered keys (F1/F2/F3/ESC). */
    int prev_f1 = 0, prev_f2 = 0, prev_f3 = 0, prev_esc = 0;

    /* Handle a menu bar action. Returns 0 to quit. */
    /* (Defined as a lambda-like via goto-free helper below the loop.) */

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            else if (ev.type == SDL_MOUSEMOTION) {
                pc_menu_bar_hover(ev.motion.x, ev.motion.y);
            }
            else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                MenuAction a = pc_menu_bar_click(ev.button.x, ev.button.y);
                if (a == MENU_FILE_OPEN_ROM) {
                    char path[1024];
                    if (open_rom_dialog(path, sizeof(path))) {
                        if (pc_gba_load_rom(path)) {
                            rom_loaded = 1;
                            pc_menu_bar_set_rom_loaded(1);
                            paused = 0;
                            printf("PC: ROM loaded: %s\n", pc_gba_title());
                            pc_link_attach_if_active(rom_loaded);
                        } else {
                            printf("PC: Failed to load ROM: %s\n", path);
                        }
                    }
                }
                else if (a == MENU_FILE_EXIT) running = 0;
                else if (a == MENU_EMU_PAUSE) paused = !paused;
                else if (a == MENU_EMU_RESET && rom_loaded) {
                    pc_gba_reset();
                    if (audio_dev) SDL_ClearQueuedAudio(audio_dev);
                    paused = 0;
                    ff_mode = 0;
                    pc_menu_bar_set_ff_mode(0);
                    printf("PC: ROM reset\n");
                }
                else if (a == MENU_EMU_SAVE_STATE && rom_loaded) {
                    if (gba_state_save() == 0) printf("PC: state saved\n");
                }
                else if (a == MENU_EMU_LOAD_STATE && rom_loaded) {
                    if (gba_state_load() == 0) printf("PC: state loaded\n");
                }
                else if (a == MENU_EMU_FF_2X) {
                    ff_mode = (ff_mode == 1) ? 0 : 1;  /* toggle */
                    pc_menu_bar_set_ff_mode(ff_mode);
                    printf("PC: fast-forward %s\n", ff_mode ? "2x" : "OFF");
                }
                else if (a == MENU_EMU_FF_4X) {
                    ff_mode = (ff_mode == 2) ? 0 : 2;  /* toggle */
                    pc_menu_bar_set_ff_mode(ff_mode);
                    printf("PC: fast-forward %s\n", ff_mode ? "4x" : "OFF");
                }
                else if (a == MENU_OPT_SCREEN) {
                    g_fullscreen = !g_fullscreen;
                    printf("PC: screen %s\n", g_fullscreen ? "16:9" : "3:2");
                }
                else if (a == MENU_OPT_FULLSCREEN) {
                    is_fullscreen = !is_fullscreen;
                    SDL_SetWindowFullscreen(window,
                        is_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    printf("PC: fullscreen %s\n", is_fullscreen ? "ON" : "OFF");
                }
                else if (a == MENU_OPT_VOLUME_UP) {
                    extern int g_volume;
                    if (g_volume < 10) { g_volume++; printf("PC: volume %d\n", g_volume); }
                }
                else if (a == MENU_OPT_VOLUME_DOWN) {
                    extern int g_volume;
                    if (g_volume > 0) { g_volume--; printf("PC: volume %d\n", g_volume); }
                }
                else if (a == MENU_HELP_ABOUT) {
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION,
                        "About GbaC0re",
                        "GbaC0re PC v1.2\nGBA emulator (mGBA core)\n"
                        "Link-cable bridge: host or join over UDP.",
                        window);
                }
                else if (a == MENU_LINK_HOST) link_do_host(rom_loaded);
                else if (a == MENU_LINK_JOIN) link_do_join(renderer, window, rom_loaded);
                else if (a == MENU_LINK_DISCONNECT) link_do_disconnect();
            }
            else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_o &&
                     (ev.key.keysym.mod & KMOD_CTRL)) {
                /* Ctrl+O: open ROM (works with or without a ROM loaded). */
                char path[1024];
                if (open_rom_dialog(path, sizeof(path))) {
                    if (pc_gba_load_rom(path)) {
                        rom_loaded = 1;
                        pc_menu_bar_set_rom_loaded(1);
                        paused = 0;
                        pc_link_attach_if_active(rom_loaded);
                    }
                }
            }
        }

        const Uint8 *k = SDL_GetKeyboardState(NULL);

        /* Edge-triggered function keys. */
        int f1 = k[SDL_SCANCODE_F1];
        int f2 = k[SDL_SCANCODE_F2];
        int f3 = k[SDL_SCANCODE_F3];
        int esc = k[SDL_SCANCODE_ESCAPE];

        if (f1 && !prev_f1 && rom_loaded) {
            if (gba_state_save() == 0) printf("PC: state saved\n");
        }
        if (f2 && !prev_f2 && rom_loaded) {
            if (gba_state_load() == 0) printf("PC: state loaded\n");
        }
        if (f3 && !prev_f3 && rom_loaded) {
            ff_mode = (ff_mode + 1) % 3;
            pc_menu_bar_set_ff_mode(ff_mode);
            printf("PC: fast-forward %s\n",
                   ff_mode == 0 ? "OFF" : (ff_mode == 1 ? "2x" : "4x"));
        }
        if (esc && !prev_esc && rom_loaded) {
            /* Wait for release, then open menu. */
            while (k[SDL_SCANCODE_ESCAPE]) {
                SDL_PumpEvents();
                k = SDL_GetKeyboardState(NULL);
                SDL_Delay(10);
            }
            if (!run_menu()) running = 0;
        }
        prev_f1 = f1; prev_f2 = f2; prev_f3 = f3; prev_esc = esc;

        bridge_poll();  /* link-cable pump: answer master REQs, update status */

        /* Gamepad menu: Start+Select. */
        u16 gp = pad_buttons();
        if ((gp & GBA_BTN_START) && (gp & GBA_BTN_SELECT)) {
            /* Wait for release. */
            while (pad_buttons() & (GBA_BTN_START | GBA_BTN_SELECT))
                SDL_Delay(10);
            if (!run_menu()) running = 0;
        }

        u16 btn = 0;
        if (rom_loaded && !paused && !pc_menu_bar_dropdown_open()) {
            btn = keyboard_buttons() | pad_buttons();
        }
        pc_gba_set_input(btn);

        if (rom_loaded && !paused) {
            /* Run frames (fast-forward = multiple frames). */
            int frames = (ff_mode == 0) ? 1 : (ff_mode == 1 ? 2 : 4);
            for (int i = 0; i < frames; i++) {
                pc_gba_run_frame();
            }
            if (ff_mode == 0) {
                pc_gba_audio_flush();
            } else {
                /* FF chipmunk: decimate for pitch-up, no freeze. */
                pc_gba_audio_flush_chipmunk(ff_mode == 1 ? 2 : 4);
            }
            present_game();
        } else if (!rom_loaded) {
            /* Welcome screen: dark background with centered prompt. */
            SDL_SetRenderDrawColor(renderer, 15, 20, 16, 255);
            SDL_RenderClear(renderer);
            {
                int ww, wh;
                SDL_GetWindowSize(window, &ww, &wh);
                const char *t1 = "GBAC0RE";
                const char *t2 = "FILE > OPEN ROM  (CTRL+O)";
                pc_menu_text((ww - pc_menu_text_width(t1)) / 2, wh / 2 - 40,
                             t1, 0, 221, 0);
                pc_menu_text((ww - pc_menu_text_width(t2)) / 2, wh / 2,
                             t2, 160, 160, 160);
            }
            pc_menu_bar_render();
            SDL_RenderPresent(renderer);
        } else {
            /* Paused: keep last frame, just render menu bar. */
            present_game();
        }

        /* Audio is the master clock: throttle emulation to keep the SDL
           queue near ~3 frames. Vsync alone can't do this on high-refresh
           displays (e.g. 144Hz), where the loop outruns real-time and the
           audio queue grows unbounded -> lagging, out-of-sync sound. */
        if (ff_mode == 0) {
            const Uint32 target = (48000 / 60) * 3 * 4; /* 3 frames s16 stereo */
            while (SDL_GetQueuedAudioSize(audio_dev) > target) {
                SDL_Delay(1);
            }
        }

        /* Cap at ~60fps when not fast-forwarding (vsync handles it if
           enabled; this is a fallback). */
        if (ff_mode == 0) {
            /* SDL_RENDERER_PRESENTVSYNC should handle this. */
        }

        prev_btn = btn;
    }

    pc_gba_unload();
    if (pad) SDL_GameControllerClose(pad);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    SDL_DestroyTexture(game_tex);
    SDL_DestroyTexture(ui_tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
