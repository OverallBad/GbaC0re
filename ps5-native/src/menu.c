#include "menu.h"
#include "ui.h"
#include "bridge.h"  /* bridge_status_text() for the live LINK label */

static const char *menu_items[MENU_NITEMS] = {
    "RESUME",
    "SAVE STATE",
    "LOAD STATE",
    "CHANGE ROM",
    "OPTIONS",
    "LINK",  /* label replaced dynamically in menu_draw */
    "QUIT",  /* always last */
};

static const char *opt_items[OPT_NITEMS] = {
    "SCREEN: 16:9",  /* label replaced dynamically in menu_draw */
    "COLOR: LIME",   /* label replaced dynamically in menu_draw */
    "VOLUME: 10",    /* label replaced dynamically in menu_draw */
    "SAVE GAME",
    "BACK",
};

int g_menu_level = 0;
int g_volume = 10;

int g_fullscreen = 1;

int g_accent_idx = 0;
const u32 accent_colors[ACCENT_NCOLORS] = {
    0xFF00DD00u, /* GREEN  */
    0xFFAA00FFu, /* PURPLE */
    0xFFFF69B4u, /* PINK   */
    0xFFFF2222u, /* RED    */
    0xFFFFE600u, /* YELLOW */
    0xFF0A84FFu, /* BLUE   */
};
const char *accent_names[ACCENT_NCOLORS] = {
    "GREEN", "PURPLE", "PINK", "RED", "YELLOW", "BLUE",
};

void menu_panel(u32 *ui, int x, int y, int w, int h) {
    ui_shadow(ui, x, y, w, h, 10);
    ui_rrect(ui, x, y, w, h, 10, M_COL_BORDER);
    ui_rrect(ui, x + 1, y + 1, w - 2, h - 2, 9, M_COL_PANEL);
}

/* Copies at most max-1 chars, uppercases (the font is caps-only). */
static void title_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[i] = c;
        i++;
    }
    dst[i] = '\0';
}

void menu_draw(u32 *ui, const char *rom_title, int cursor,
               const char *toast_msg, int toast_frames) {
    int px = 110, py = 30, pw = 260, ph = 230;
    int cx = px + pw / 2;

    /* No drop shadow and no translucent fills: every pixel emitted here is
       opaque or transparent, so the final blit_ui_blend never reads the
       video framebuffer. Reads from that write-combining mapping were the
       v1.1.x pause-menu lag (the ROM picker was always fast because its
       blit path is write-only). */
    ui_rrect(ui, px, py, pw, ph, 10, M_COL_BORDER);
    ui_rrect(ui, px + 1, py + 1, pw - 2, ph - 2, 9, 0xFF141926u);

    draw_centered_scale(ui, cx, py + 14, "GBAC0RE",
                        accent_colors[g_accent_idx], 2);

    char t[32];
    title_copy(t, rom_title ? rom_title : "", sizeof t);
    draw_centered_scale(ui, cx, py + 36, t, M_COL_SUB, 1);

    ui_rect_alpha(ui, px + 16, py + 50, pw - 32, 1, M_COL_BORDER);

    /* Scrollable item list: 5 visible, the panel keeps a clean layout no
       matter how many items exist. The scroll window follows the cursor.
       Level 0 = main menu, level 1 = options submenu. */
#define MENU_VISIBLE 5
    int nitems = (g_menu_level == 0) ? MENU_NITEMS : OPT_NITEMS;
    const char **items = (g_menu_level == 0) ? menu_items : opt_items;
    int scroll = 0;
    if (cursor >= MENU_VISIBLE) scroll = cursor - MENU_VISIBLE + 1;
    if (scroll > nitems - MENU_VISIBLE) scroll = nitems - MENU_VISIBLE;
    if (scroll < 0) scroll = 0;

    int iy = py + 60;
    for (int vi = 0; vi < MENU_VISIBLE && (scroll + vi) < nitems; vi++) {
        int i = scroll + vi;
        int sel = (i == cursor);
        if (sel)
            ui_rrect(ui, px + 14, iy - 4, pw - 28, 24, 6,
                     accent_colors[g_accent_idx]);
        /* Options level: items 0, 1, 2 are live settings. */
        const char *label = items[i];
        char dyn_label[32];
        if (g_menu_level == 0 && i == 5) {
            /* LINK item: live status ("LINK: OFF", "LINK: HOST (WAITING)",
               "LINK: HOST (CONNECTED)"). */
            const char *st = bridge_status_text();
            int k = 0;
            while (*st && k < 31) dyn_label[k++] = *st++;
            dyn_label[k] = 0;
            label = dyn_label;
        } else if (g_menu_level == 1 && i == 0) {
            const char *mode = g_fullscreen ? "16:9" : "3:2";
            int k = 0;
            const char *p = "SCREEN: ";
            while (*p && k < 15) dyn_label[k++] = *p++;
            while (*mode && k < 15) dyn_label[k++] = *mode++;
            dyn_label[k] = 0;
            label = dyn_label;
        } else if (g_menu_level == 1 && i == 1) {
            const char *name = accent_names[g_accent_idx];
            int k = 0;
            const char *p = "COLOR: ";
            while (*p && k < 15) dyn_label[k++] = *p++;
            while (*name && k < 15) dyn_label[k++] = *name++;
            dyn_label[k] = 0;
            label = dyn_label;
        } else if (g_menu_level == 1 && i == 2) {
            int k = 0;
            const char *p = "VOLUME: ";
            while (*p && k < 15) dyn_label[k++] = *p++;
            if (g_volume >= 10) {
                dyn_label[k++] = '1'; dyn_label[k++] = '0';
            } else {
                dyn_label[k++] = '0' + g_volume;
            }
            dyn_label[k] = 0;
            label = dyn_label;
        }
        draw_str_scale(ui, px + 30, iy, label,
                       sel ? M_COL_PILLTXT : M_COL_TEXT, 2);
        iy += 26;
    }
#undef MENU_VISIBLE

    draw_centered_scale(ui, cx, py + ph - 16,
                        "CROSS SELECT   CIRCLE BACK", M_COL_HINT, 1);

    if (toast_frames > 0) {
        /* Inside the panel, above the hint line: the 480x270 UI buffer ends
           at y=269, so the old y=270 toast was off-screen (v1.2.0/1.2.1
           regression -- the toast never appeared). */
        int tw = 132, tx = 240 - tw / 2, ty = py + ph - 44;
        ui_rrect(ui, tx, ty, tw, 20, 10, 0xFF14181Eu);
        draw_centered_scale(ui, 240, ty + 6,
                            toast_msg ? toast_msg : "SAVED",
                            accent_colors[g_accent_idx], 1);
    }
}
