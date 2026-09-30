#include "ui.h"
#include "tables.h"

int str_len(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

void ui_fill(u32 *scr, u32 color) {
    for (int i = 0; i < UI_W * UI_H; i++) scr[i] = color;
}

void clear_fb(u32 *fb) {
    for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF000000u;
}

void draw_char(u32 *scr, int x, int y, char ch, u32 color) {
    int idx = 0;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch >= 32 && ch <= 90) idx = ch - 32;
    const u8 *glyph = font_data[idx];

    for (int r = 0; r < 8; r++) {
        u8 bits = glyph[r];
        for (int c = 0; c < 8; c++) {
            if (bits & (0x80 >> c)) {
                int px = x + c, py = y + r;
                if (px >= 0 && px < UI_W && py >= 0 && py < UI_H)
                    scr[py * UI_W + px] = color;
            }
        }
    }
}

void draw_str(u32 *scr, int x, int y, const char *s, u32 color) {
    while (*s) {
        draw_char(scr, x, y, *s, color);
        x += 8;
        s++;
    }
}

void draw_centered(u32 *scr, int y, const char *s, u32 color) {
    int x = (UI_W - str_len(s) * 8) / 2;
    if (x < 0) x = 0;
    draw_str(scr, x, y, s, color);
}

void draw_hline(u32 *scr, int y, int x1, int x2, u32 color) {
    if (y < 0 || y >= UI_H) return;
    for (int x = x1; x < x2 && x < UI_W; x++) {
        if (x >= 0) scr[y * UI_W + x] = color;
    }
}

/* UI_W * UI_SCALE == SCR_W and UI_H * UI_SCALE == SCR_H exactly, so this
   writes every pixel of the frame and never needs clear_fb() first. */
void blit_ui(u32 *fb, const u32 *src) {
    for (int uy = 0; uy < UI_H; uy++) {
        const u32 *srow = &src[uy * UI_W];
        for (int dy = 0; dy < UI_SCALE; dy++) {
            u32 *row = &fb[(uy * UI_SCALE + dy) * SCR_W];
            for (int ux = 0; ux < UI_W; ux++) {
                u32 c = srow[ux];
                u32 *out = &row[ux * UI_SCALE];
                for (int dx = 0; dx < UI_SCALE; dx++) out[dx] = c;
            }
        }
    }
}

void blit_scale(u32 *fb, const u32 *src) {
    for (int gy = 0; gy < GBACORE_H; gy++) {
        int sy = OFF_Y + gy * SCALE;
        const u32 *srow = &src[gy * GBACORE_W];
        for (int gx = 0; gx < GBACORE_W; gx++) {
            u32 c  = srow[gx];
            int sx = OFF_X + gx * SCALE;
            for (int dy = 0; dy < SCALE; dy++) {
                u32 *row = &fb[(sy + dy) * SCR_W + sx];
                for (int dx = 0; dx < SCALE; dx++) row[dx] = c;
            }
        }
    }
}

/* Fullscreen 240x160 -> 1920x1080. X scale is exactly 8; Y is 6.75, so
   source rows map to 6 or 7 destination rows (dy0/dy1 via integer math).
   Fills the entire framebuffer -- no bars. */
void blit_fullscreen(u32 *fb, const u32 *src) {
    for (int sy = 0; sy < GBACORE_H; sy++) {
        int dy0 = (sy * SCR_H) / GBACORE_H;
        int dy1 = ((sy + 1) * SCR_H) / GBACORE_H;
        const u32 *srow = &src[sy * GBACORE_W];
        for (int dy = dy0; dy < dy1; dy++) {
            u32 *drow = &fb[dy * SCR_W];
            for (int sx = 0; sx < GBACORE_W; sx++) {
                u32 c = srow[sx];
                u32 *out = &drow[sx * 8];
                for (int dx = 0; dx < 8; dx++) out[dx] = c;
            }
        }
    }
}

static char lower_ch(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

int is_rom_file(const char *name) {
    int len = str_len(name);
    if (len < 5) return 0;

    /* .gba only -- this build is the GBA core. */
    if (name[len - 4] == '.') {
        char a = lower_ch(name[len - 3]);
        char b = lower_ch(name[len - 2]);
        char c = lower_ch(name[len - 1]);
        if (a == 'g' && b == 'b' && c == 'a') return 1;
    }
    return 0;
}

void extract_rom_name(const char *fn, char *out, int max) {
    int i = 0;
    while (fn[i] && fn[i] != '.' && i < max - 1) {
        out[i] = fn[i];
        i++;
    }
    out[i] = '\0';
}

/* ------------------------- modern UI primitives (v1.1.0) ----------------- */

void ui_clear(u32 *scr) {
    for (int i = 0; i < UI_W * UI_H; i++) scr[i] = 0;
}

static void blend_pixel(u32 *dst, u32 src) {
    u32 a = src >> 24;
    if (a == 0) return;
    if (a == 255) { *dst = src | 0xFF000000u; return; }
    u32 d = *dst;
    u32 sr = (src >> 16) & 255, sg = (src >> 8) & 255, sb = src & 255;
    u32 dr = (d >> 16) & 255,   dg = (d >> 8) & 255,   db = d & 255;
    u32 ia = 255 - a;
    u32 r = (sr * a + dr * ia) / 255;
    u32 g = (sg * a + dg * ia) / 255;
    u32 b = (sb * a + db * ia) / 255;
    *dst = 0xFF000000u | (r << 16) | (g << 8) | b;
}

void ui_rect_alpha(u32 *scr, int x, int y, int w, int h, u32 color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > UI_W) w = UI_W - x;
    if (y + h > UI_H) h = UI_H - y;
    if (w <= 0 || h <= 0) return;
    for (int py = y; py < y + h; py++) {
        u32 *row = &scr[py * UI_W + x];
        for (int px = 0; px < w; px++) blend_pixel(&row[px], color);
    }
}

/* Filled rounded rect. Corner quarter-circles of radius r are centred at
   (r,r), (w-r,r), (r,h-r), (w-r,h-r) in rect-local coords; a pixel is in
   when its squared distance to the nearest corner centre is <= r*r. */
void ui_rrect(u32 *scr, int x, int y, int w, int h, int r, u32 color) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= UI_H) continue;
        for (int px = x; px < x + w; px++) {
            if (px < 0 || px >= UI_W) continue;
            int dx = px - x, dy = py - y, inside;
            if (dx >= r && dx < w - r) inside = 1;
            else if (dy >= r && dy < h - r) inside = 1;
            else {
                int qx = (dx < r) ? dx : w - 1 - dx;
                int qy = (dy < r) ? dy : h - 1 - dy;
                int cx = r - qx, cy = r - qy;
                inside = (cx * cx + cy * cy <= r * r);
            }
            if (inside) blend_pixel(&scr[py * UI_W + px], color);
        }
    }
}

void ui_shadow(u32 *scr, int x, int y, int w, int h, int r) {
    ui_rrect(scr, x - 5, y - 1, w + 10, h + 10, r + 5, 0x30000000u);
    ui_rrect(scr, x - 2, y + 1, w + 4, h + 4, r + 2, 0x50000000u);
}

void dim_fb(u32 *fb, int amt) {
    if (amt < 0) amt = 0;
    if (amt > 256) amt = 256;
    for (int i = 0; i < SCR_W * SCR_H; i++) {
        u32 c = fb[i];
        u32 r = (((c >> 16) & 255) * (u32)amt) >> 8;
        u32 g = (((c >> 8) & 255) * (u32)amt) >> 8;
        u32 b = ((c & 255) * (u32)amt) >> 8;
        fb[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

/* Same dim, but over an explicit pixel count. The pause menu (v1.1.3) dims
   the frozen game frame at GBA resolution in cached RAM instead of dimming
   the video framebuffer in place: reads from that write-combining mapping
   were the v1.1.x menu lag, so nothing in the menu path reads video memory
   anymore. */
void dim_buf(u32 *fb, int n, int amt) {
    if (amt < 0) amt = 0;
    if (amt > 256) amt = 256;
    for (int i = 0; i < n; i++) {
        u32 c = fb[i];
        u32 r = (((c >> 16) & 255) * (u32)amt) >> 8;
        u32 g = (((c >> 8) & 255) * (u32)amt) >> 8;
        u32 b = ((c & 255) * (u32)amt) >> 8;
        fb[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

void blit_ui_blend(u32 *fb, const u32 *src) {
    for (int uy = 0; uy < UI_H; uy++) {
        const u32 *srow = &src[uy * UI_W];
        for (int dy = 0; dy < UI_SCALE; dy++) {
            u32 *row = &fb[(uy * UI_SCALE + dy) * SCR_W];
            for (int ux = 0; ux < UI_W; ux++) {
                u32 c = srow[ux];
                u32 a = c >> 24;
                if (a == 0) continue;
                u32 *out = &row[ux * UI_SCALE];
                if (a == 255) {
                    c |= 0xFF000000u;
                    for (int dx = 0; dx < UI_SCALE; dx++) out[dx] = c;
                } else {
                    for (int dx = 0; dx < UI_SCALE; dx++) blend_pixel(&out[dx], c);
                }
            }
        }
    }
}

static void draw_glyph_scale(u32 *scr, int x, int y, char ch, u32 color, int sc) {
    int idx = 0;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch >= 32 && ch <= 90) idx = ch - 32;
    const u8 *glyph = font_data[idx];

    for (int r = 0; r < 8; r++) {
        u8 bits = glyph[r];
        for (int c = 0; c < 8; c++) {
            if (bits & (0x80 >> c)) {
                int gx = x + c * sc, gy = y + r * sc;
                for (int dy = 0; dy < sc; dy++) {
                    int py = gy + dy;
                    if (py < 0 || py >= UI_H) continue;
                    for (int dx = 0; dx < sc; dx++) {
                        int px = gx + dx;
                        if (px < 0 || px >= UI_W) continue;
                        blend_pixel(&scr[py * UI_W + px], color);
                    }
                }
            }
        }
    }
}

void draw_str_scale(u32 *scr, int x, int y, const char *s, u32 color, int sc) {
    if (sc < 1) sc = 1;
    if (sc > 4) sc = 4;
    while (*s) {
        draw_glyph_scale(scr, x, y, *s, color, sc);
        x += 8 * sc;
        s++;
    }
}

void draw_centered_scale(u32 *scr, int cx, int y, const char *s, u32 color, int sc) {
    int w = str_len(s) * 8 * sc;
    int x = cx - w / 2;
    if (x < 0) x = 0;
    draw_str_scale(scr, x, y, s, color, sc);
}
