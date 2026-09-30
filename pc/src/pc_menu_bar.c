/* GbaC0re PC — top menu bar implementation.
 * Custom SDL rendering using the 8x8 bitmap font from tables.h.
 * 28px high bar, dropdown menus, mouse hover/click. */

#include "pc_menu_bar.h"
#include "tables.h"  /* font_data */
#include "bridge.h"  /* bridge_status_text() for the live status item */

#include <string.h>
#include <stdio.h>

#define FONT_W 8
#define FONT_H 8
#define TEXT_SCALE 2  /* 16px text in 28px bar */

/* Colors */
static const SDL_Color COL_BAR_BG   = { 32,  32,  32, 255 };
static const SDL_Color COL_BAR_HOVER= { 64,  64,  64, 255 };
static const SDL_Color COL_TEXT      = { 220, 220, 220, 255 };
static const SDL_Color COL_TEXT_DIM  = { 120, 120, 120, 255 };
static const SDL_Color COL_DROP_BG   = { 40,  40,  40, 255 };
static const SDL_Color COL_DROP_HOVER= { 70,  90, 130, 255 };
static const SDL_Color COL_BORDER    = { 80,  80,  80, 255 };

static SDL_Renderer *g_ren = NULL;
static SDL_Window *g_win = NULL;
static int g_win_w = 0, g_win_h = 0;
static int g_rom_loaded = 0;
static int g_ff_mode = 0;

extern int g_fullscreen;  /* menu.c: 1 = 16:9 stretch, 0 = 3:2 integer */

/* Dynamic labels (font is caps-only, ASCII 32-90). */
static char screen_label[16] = "SCREEN: 16:9";
static char ff2_label[24] = "FAST FORWARD 2X";
static char ff4_label[24] = "FAST FORWARD 4X";
static char link_status_label[48] = "LINK: OFF";

/* Menu structure */
typedef struct {
    const char *title;
    int x, w;  /* computed layout */
} TopMenu;

typedef struct {
    MenuAction action;
    const char *label;
    int enabled;
} MenuItem;

/* Menus */
static TopMenu top_menus[] = {
    { "File", 0, 0 },
    { "Emulation", 0, 0 },
    { "Options", 0, 0 },
    { "Link", 0, 0 },
    { "Help", 0, 0 },
};
#define N_TOP (sizeof(top_menus)/sizeof(top_menus[0]))

static MenuItem file_items[] = {
    { MENU_FILE_OPEN_ROM, "OPEN ROM...    CTRL+O", 1 },
    { MENU_FILE_EXIT,     "EXIT", 1 },
};
static MenuItem emu_items[] = {
    { MENU_EMU_PAUSE,      "PAUSE/RESUME", 0 },
    { MENU_EMU_RESET,      "RESET", 0 },
    { MENU_EMU_SAVE_STATE, "SAVE STATE  F1", 0 },
    { MENU_EMU_LOAD_STATE, "LOAD STATE  F2", 0 },
    { MENU_EMU_FF_2X,      ff2_label, 0 },
    { MENU_EMU_FF_4X,      ff4_label, 0 },
};
static MenuItem opt_items[] = {
    { MENU_OPT_SCREEN,     screen_label, 1 },
    { MENU_OPT_FULLSCREEN, "FULLSCREEN", 1 },
    { MENU_OPT_VOLUME_UP,   "VOLUME UP", 1 },
    { MENU_OPT_VOLUME_DOWN, "VOLUME DOWN", 1 },
};
static MenuItem help_items[] = {
    { MENU_HELP_ABOUT, "About GbaC0re", 1 },
};
static MenuItem link_items[] = {
    { MENU_LINK_HOST,       "HOST LINK GAME", 1 },
    { MENU_LINK_JOIN,       "JOIN LINK GAME...", 1 },
    { MENU_LINK_DISCONNECT, "DISCONNECT", 1 },
    { MENU_NONE,            link_status_label, 0 },
};

static int open_menu = -1;  /* index into top_menus, -1 = none */
static int hover_top = -1;
static int hover_item = -1;

/* Render a string using the 8x8 font, scaled. Returns width in pixels.
 * NOTE: font_data only covers ASCII 32-90 (see ui.c draw_char): lowercase
 * is uppercased and anything outside 32-90 becomes '?'. Indexing past 90
 * reads out of bounds and renders garbage. */
static int render_text(int x, int y, const char *s, SDL_Color col) {
    int ox = x;
    SDL_SetRenderDrawColor(g_ren, col.r, col.g, col.b, col.a);
    for (const char *p = s; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 90) ch = '?';
        const u8 *glyph = font_data[ch - 32];
        for (int gy = 0; gy < FONT_H; gy++) {
            for (int gx = 0; gx < FONT_W; gx++) {
                if (glyph[gy] & (1 << (7 - gx))) {
                    SDL_Rect r = { ox + gx * TEXT_SCALE, y + gy * TEXT_SCALE,
                                   TEXT_SCALE, TEXT_SCALE };
                    SDL_RenderFillRect(g_ren, &r);
                }
            }
        }
        ox += FONT_W * TEXT_SCALE;
    }
    return ox - x;
}

static int text_width(const char *s) {
    int n = 0;
    for (const char *p = s; *p; p++) n++;
    return n * FONT_W * TEXT_SCALE;
}

void pc_menu_text(int x, int y, const char *s, int r, int g, int b) {
    if (!g_ren) return;
    SDL_Color col = { (Uint8)r, (Uint8)g, (Uint8)b, 255 };
    render_text(x, y, s, col);
}

int pc_menu_text_width(const char *s) {
    return text_width(s);
}

void pc_menu_bar_set_ff_mode(int mode) {
    g_ff_mode = mode;
    snprintf(ff2_label, sizeof ff2_label, "FAST FORWARD 2X%s",
             mode == 1 ? " (ON)" : "");
    snprintf(ff4_label, sizeof ff4_label, "FAST FORWARD 4X%s",
             mode == 2 ? " (ON)" : "");
}

void pc_menu_bar_init(SDL_Renderer *renderer, SDL_Window *window, int win_w, int win_h) {
    g_ren = renderer;
    g_win = window;
    g_win_w = win_w;
    g_win_h = win_h;
    /* Layout top menus */
    int x = 8;
    for (size_t i = 0; i < N_TOP; i++) {
        top_menus[i].x = x;
        top_menus[i].w = text_width(top_menus[i].title) + 16;
        x += top_menus[i].w;
    }
}

void pc_menu_bar_set_rom_loaded(int loaded) {
    g_rom_loaded = loaded;
    for (size_t i = 0; i < sizeof(emu_items)/sizeof(emu_items[0]); i++) {
        emu_items[i].enabled = loaded;
    }
}

static MenuItem* get_items(int menu_idx, int *count) {
    switch (menu_idx) {
        case 0: *count = sizeof(file_items)/sizeof(file_items[0]); return file_items;
        case 1: *count = sizeof(emu_items)/sizeof(emu_items[0]); return emu_items;
        case 2: *count = sizeof(opt_items)/sizeof(opt_items[0]); return opt_items;
        case 3: *count = sizeof(link_items)/sizeof(link_items[0]); return link_items;
        case 4: *count = sizeof(help_items)/sizeof(help_items[0]); return help_items;
    }
    *count = 0;
    return NULL;
}

void pc_menu_bar_render(void) {
    if (!g_ren) return;

    /* Live window size: the bar must fill the window after resizing. */
    if (g_win) SDL_GetWindowSize(g_win, &g_win_w, &g_win_h);

    /* Keep the SCREEN label in sync (toggled from pc_main). */
    snprintf(screen_label, sizeof screen_label, "SCREEN: %s",
             g_fullscreen ? "16:9" : "3:2");

    /* Live link status (updated by bridge_poll as the handshake lands). */
    snprintf(link_status_label, sizeof link_status_label, "%s",
             bridge_status_text());

    /* Link actions follow the session state. */
    {
        int active = bridge_role() != LINK_ROLE_NONE;
        link_items[0].enabled = !active;  /* HOST */
        link_items[1].enabled = !active;  /* JOIN */
        link_items[2].enabled = active;   /* DISCONNECT */
    }

    /* Bar background */
    SDL_SetRenderDrawColor(g_ren, COL_BAR_BG.r, COL_BAR_BG.g, COL_BAR_BG.b, 255);
    SDL_Rect bar = { 0, 0, g_win_w, MENU_BAR_H };
    SDL_RenderFillRect(g_ren, &bar);

    /* Top-level menus */
    for (size_t i = 0; i < N_TOP; i++) {
        int is_hover = (hover_top == (int)i);
        int is_open = (open_menu == (int)i);
        if (is_hover || is_open) {
            SDL_SetRenderDrawColor(g_ren, COL_BAR_HOVER.r, COL_BAR_HOVER.g, COL_BAR_HOVER.b, 255);
            SDL_Rect r = { top_menus[i].x - 8, 0, top_menus[i].w, MENU_BAR_H };
            SDL_RenderFillRect(g_ren, &r);
        }
        render_text(top_menus[i].x, 6, top_menus[i].title, COL_TEXT);
    }

    /* Dropdown */
    if (open_menu >= 0) {
        int count;
        MenuItem *items = get_items(open_menu, &count);
        int max_w = 0;
        for (int i = 0; i < count; i++) {
            int w = text_width(items[i].label);
            if (w > max_w) max_w = w;
        }
        max_w += 20;
        int dd_x = top_menus[open_menu].x - 8;
        int dd_y = MENU_BAR_H;
        int dd_h = count * 24 + 8;

        /* Background + border */
        SDL_SetRenderDrawColor(g_ren, COL_DROP_BG.r, COL_DROP_BG.g, COL_DROP_BG.b, 255);
        SDL_Rect bg = { dd_x, dd_y, max_w, dd_h };
        SDL_RenderFillRect(g_ren, &bg);
        SDL_SetRenderDrawColor(g_ren, COL_BORDER.r, COL_BORDER.g, COL_BORDER.b, 255);
        SDL_RenderDrawRect(g_ren, &bg);

        /* Items */
        for (int i = 0; i < count; i++) {
            int iy = dd_y + 4 + i * 24;
            if (hover_item == i) {
                SDL_SetRenderDrawColor(g_ren, COL_DROP_HOVER.r, COL_DROP_HOVER.g, COL_DROP_HOVER.b, 255);
                SDL_Rect hr = { dd_x + 2, iy, max_w - 4, 22 };
                SDL_RenderFillRect(g_ren, &hr);
            }
            SDL_Color tc = items[i].enabled ? COL_TEXT : COL_TEXT_DIM;
            render_text(dd_x + 10, iy + 3, items[i].label, tc);
        }
    }
}

MenuAction pc_menu_bar_click(int x, int y) {
    /* Click in dropdown? */
    if (open_menu >= 0) {
        int count;
        MenuItem *items = get_items(open_menu, &count);
        int max_w = 0;
        for (int i = 0; i < count; i++) {
            int w = text_width(items[i].label);
            if (w > max_w) max_w = w;
        }
        max_w += 20;
        int dd_x = top_menus[open_menu].x - 8;
        int dd_y = MENU_BAR_H;
        if (x >= dd_x && x < dd_x + max_w && y >= dd_y) {
            int idx = (y - dd_y - 4) / 24;
            if (idx >= 0 && idx < count && items[idx].enabled) {
                MenuAction a = items[idx].action;
                open_menu = -1;
                hover_item = -1;
                return a;
            }
        }
        /* Click outside closes */
        open_menu = -1;
        hover_item = -1;
        return MENU_NONE;
    }

    /* Click on top menu? */
    if (y < MENU_BAR_H) {
        for (size_t i = 0; i < N_TOP; i++) {
            if (x >= top_menus[i].x - 8 && x < top_menus[i].x - 8 + top_menus[i].w) {
                open_menu = (int)i;
                hover_item = -1;
                return MENU_NONE;
            }
        }
    }
    return MENU_NONE;
}

void pc_menu_bar_hover(int x, int y) {
    hover_top = -1;
    if (y < MENU_BAR_H) {
        for (size_t i = 0; i < N_TOP; i++) {
            if (x >= top_menus[i].x - 8 && x < top_menus[i].x - 8 + top_menus[i].w) {
                hover_top = (int)i;
                break;
            }
        }
        /* Switch open menu on hover */
        if (open_menu >= 0 && hover_top >= 0 && hover_top != open_menu) {
            open_menu = hover_top;
            hover_item = -1;
        }
    } else if (open_menu >= 0) {
        /* Hover in dropdown */
        int count;
        get_items(open_menu, &count);
        int dd_y = MENU_BAR_H;
        int idx = (y - dd_y - 4) / 24;
        hover_item = (idx >= 0 && idx < count) ? idx : -1;
    }
}

int pc_menu_bar_dropdown_open(void) {
    return open_menu >= 0;
}

void pc_menu_bar_close(void) {
    open_menu = -1;
    hover_item = -1;
}
