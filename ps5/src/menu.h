#ifndef GBACORE_MENU_H
#define GBACORE_MENU_H

#include "core.h"

/* Pause menu actions returned by the menu loop in main.c. */
#define MENU_RESUME 0   /* close the menu, keep playing */
#define MENU_ROMS   1   /* save, tear down, back to the ROM picker */
#define MENU_QUIT   2   /* save and exit the payload */

#define MENU_NITEMS 7   /* main: RESUME, SAVE STATE, LOAD STATE, CHANGE ROM, OPTIONS, LINK, QUIT */
#define OPT_NITEMS 5    /* options: SCREEN, COLOR, VOLUME, SAVE GAME, BACK */

/* 0 = main menu, 1 = options submenu. QUIT is always the last item of the
   main menu, no matter what gets added before it. */
extern int g_menu_level;

/* Master volume 0-10 (default 10). Applied to the 48kHz output. */
extern int g_volume;

/* 1 = stretch to 16:9 fullscreen, 0 = original 3:2 (1440x960 centered). */
extern int g_fullscreen;

/* Accent color index (highlight pill, GBAC0RE title, toast text). */
extern int g_accent_idx;
#define ACCENT_NCOLORS 6
extern const u32 accent_colors[ACCENT_NCOLORS];
extern const char *accent_names[ACCENT_NCOLORS];

/* Shared chrome: soft shadow + 1px border + translucent dark fill. */
void menu_panel(u32 *ui, int x, int y, int w, int h);

/* Draws the pause overlay into a cleared ui_screen: panel, header with the
   ROM title, the six items with the cursor pill, footer hints, and the
   toast (toast_msg, e.g. "SAVED") while toast_frames > 0. */
void menu_draw(u32 *ui, const char *rom_title, int cursor,
               const char *toast_msg, int toast_frames);

#endif
