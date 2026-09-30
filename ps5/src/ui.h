#ifndef LUAGB_UI_H
#define LUAGB_UI_H

#include "core.h"

/* The menu is drawn into a 480x270 ARGB buffer -- 16:9, our own UI, not the
   console's aspect -- and blitted by blit_ui() at an exact 4x to fill 1080p.
   Emulated frames keep their own path through blit_scale(); the two are
   deliberately separate so changing the menu cannot move the game. */

#define COL_BG     0xFF0F1410u
#define COL_BRAND  0xFF9BBC0Fu
#define COL_HEAD   0xFF8BAC0Fu
#define COL_LINE   0xFF306230u
#define COL_SEL    0xFFE0F8D0u
#define COL_NORM   0xFF9BBC0Fu
#define COL_DIM    0xFF306230u
#define COL_WARN   0xFFE8B040u

int  str_len(const char *s);
void ui_fill(u32 *scr, u32 color);
void draw_char(u32 *scr, int x, int y, char ch, u32 color);
void draw_str(u32 *scr, int x, int y, const char *s, u32 color);
void draw_centered(u32 *scr, int y, const char *s, u32 color);
void draw_hline(u32 *scr, int y, int x1, int x2, u32 color);

/* Nearest-neighbour integer scale of a 160x144 ARGB source into the centre of
   the 1920x1080 framebuffer. This is the GAME path -- do not point the menu at
   it, that is what made the menu 10:9. */
void blit_scale(u32 *fb, const u32 *src);

/* The MENU path: UI_W x UI_H scaled by UI_SCALE, which covers 1920x1080
   exactly, so there is no centring offset and no border to clear. */
void blit_ui(u32 *fb, const u32 *src);

void clear_fb(u32 *fb);
void blit_fullscreen(u32 *fb, const u32 *src);

/* Accepts .gb, .gbc and .gbs (case-insensitive). */
int  is_rom_file(const char *name);
void extract_rom_name(const char *fn, char *out, int max);

/* ------------------------- modern UI primitives (v1.1.0) ----------------- */

/* Sleek dark-theme palette. All panel colors carry alpha; draw them into a
   cleared (transparent) ui_screen and composite with blit_ui_blend(). */
#define M_COL_APPBG   0xFF0B0E13u   /* full-screen app background (opaque) */
#define M_COL_PANEL   0xF2141926u   /* menu panel fill */
#define M_COL_BORDER  0xFF2C3547u   /* 1px panel border / dividers */
#define M_COL_ACCENT  0xFF9BBC0Fu   /* brand lime: headers, selection pill */
#define M_COL_TEXT    0xFFC6CDD8u   /* unselected row text */
#define M_COL_PILLTXT 0xFF10140Au   /* text on the accent pill */
#define M_COL_SUB     0xFF7E8898u   /* subtitles, ROM names */
#define M_COL_HINT    0xFF596372u   /* footer hints */
#define M_COL_WARN    0xFFE8B040u   /* warnings */

/* Opaque black over everything (alpha ignored, kept for symmetry). */
void ui_clear(u32 *scr);

/* Filled rect / rounded rect with per-pixel src-over alpha compositing. */
void ui_rect_alpha(u32 *scr, int x, int y, int w, int h, u32 color);
void ui_rrect(u32 *scr, int x, int y, int w, int h, int r, u32 color);

/* Two-layer soft drop shadow for a panel at (x,y,w,h) radius r. */
void ui_shadow(u32 *scr, int x, int y, int w, int h, int r);

/* Multiply every pixel of the 1080p framebuffer by amt/256 (0..256). */
void dim_fb(u32 *fb, int amt);
void dim_buf(u32 *fb, int n, int amt);

/* 4x scale of the UI buffer with src-over alpha compositing, for overlays
   drawn on top of a dimmed game frame. Pixels with alpha 0 are skipped. */
void blit_ui_blend(u32 *fb, const u32 *src);

/* Text at 1x or 2x, alpha-composited so it can sit on translucent panels.
   Colors should be opaque; the glyph pixels blend like any other source. */
void draw_str_scale(u32 *scr, int x, int y, const char *s, u32 color, int sc);
void draw_centered_scale(u32 *scr, int cx, int y, const char *s, u32 color, int sc);

#endif
