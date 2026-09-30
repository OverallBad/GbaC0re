/* GbaC0re PC — traditional top menu bar (File, Emulation, Options, Help).
 * Custom SDL rendering (no native widgets). 28px high, dropdown menus.
 * Callbacks wire into pc_main.c actions. */

#ifndef PC_MENU_BAR_H
#define PC_MENU_BAR_H

#include <SDL.h>

/* Menu action IDs (returned by pc_menu_bar_click, or via callback). */
typedef enum {
    MENU_NONE = 0,
    MENU_FILE_OPEN_ROM,
    MENU_FILE_EXIT,
    MENU_EMU_PAUSE,
    MENU_EMU_RESET,
    MENU_EMU_SAVE_STATE,
    MENU_EMU_LOAD_STATE,
    MENU_EMU_FF_2X,
    MENU_EMU_FF_4X,
    MENU_OPT_VOLUME_UP,
    MENU_OPT_VOLUME_DOWN,
    MENU_OPT_SCREEN,      /* toggle 16:9 stretch / 3:2 integer */
    MENU_OPT_FULLSCREEN,  /* toggle SDL fullscreen */
    MENU_LINK_HOST,       /* host a link-cable session */
    MENU_LINK_JOIN,       /* join a host by IP */
    MENU_LINK_DISCONNECT, /* leave the link session */
    MENU_HELP_ABOUT,
} MenuAction;

/* Init with the SDL renderer and window. Must be called after init_video. */
void pc_menu_bar_init(SDL_Renderer *renderer, SDL_Window *window, int win_w, int win_h);

/* Set whether a ROM is loaded (enables/disables emulation items). */
void pc_menu_bar_set_rom_loaded(int loaded);

/* Render the menu bar (call after present_game, before SDL_RenderPresent). */
void pc_menu_bar_render(void);

/* Handle a mouse click. Returns a MenuAction (MENU_NONE if none). */
MenuAction pc_menu_bar_click(int x, int y);

/* Handle mouse motion (for hover highlight). */
void pc_menu_bar_hover(int x, int y);

/* Returns 1 if a dropdown is open (to block game input). */
int pc_menu_bar_dropdown_open(void);

/* Close any open dropdown (e.g. on ESC). */
void pc_menu_bar_close(void);

/* Height of the menu bar in pixels. */
#define MENU_BAR_H 28

/* Update the FF item labels to show which mode is active (0=off,1=2x,2=4x). */
void pc_menu_bar_set_ff_mode(int mode);

/* Draw text with the menu font (public so the welcome screen can use it). */
void pc_menu_text(int x, int y, const char *s, int r, int g, int b);
int pc_menu_text_width(const char *s);

#endif
