#ifndef CORE_H
#define CORE_H

/* -------------------------------------------------------------------------
 * GbaC0re v0.7 -- Game Boy Advance emulator running as native x86_64
 * shellcode on PS5 through the Luac0re JIT exploit.
 *
 * Emulation core: mGBA by Vicki Pfau and contributors (MPL-2.0), built with
 * MINIMAL_CORE=1 + M_CORE_GBA=1 + DISABLE_THREADING=1.
 * PS5 runtime: LuaPSX/LuaGB by soniciso1 (GPL-2.0-or-later). This file and the
 * runtime around it are derived from that work; see CREDITS.md and COPYING.
 * Delivery substrate: Luac0re by Gezine.
 * ------------------------------------------------------------------------- */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;
typedef long           s64;
typedef int            s32;
typedef short          s16;
typedef signed char    s8;

#define VERSION_STR "GbaC0re v1.2.15"

/* Offsets into the host game's eboot (Star Wars Racer Revenge).
   Same values EmuC0re/LuaPSX/LuaGB use -- they are properties of the host
   title, not of the emulator, so they carry over unchanged. */
#define GADGET_OFFSET    0x31AA9     /* the native_call trampoline gadget    */
#define LIBKERNEL_HANDLE 0x2001
#define EBOOT_GS_THREAD  0x057F89B0  /* pthread handle of the ps2emu GS thread */
#define EBOOT_VIDOUT     0x02d695d0  /* ps2emu's own sceVideoOut handle        */

/* Output geometry. */
#define SCR_W       1920
#define SCR_H       1080
#define FB_SIZE     (SCR_W * SCR_H * 4)
#define FB_ALIGNED  ((FB_SIZE + 0x1FFFFF) & ~0x1FFFFF)
#define FB_TOTAL    (FB_ALIGNED * 2)

/* GBA is 240x160. 6x integer scale -> 1440x960, centred in 1080p;
   6 is the largest factor that still fits vertically (160*6 = 960). */
#define GBA_W   240
#define GBA_H   160
#define SCALE   6
#define OFF_X   ((SCR_W - GBA_W * SCALE) / 2)
#define OFF_Y   ((SCR_H - GBA_H * SCALE) / 2)

/* The MENU is 16:9 and fills the screen; the GAME above is untouched.
 * (Same rationale as LuaGB: the picker is our UI, not emulated output.)
 *
 * 480x270 at an integer 4x is exactly 1920x1080, so the menu fills the frame
 * with no bars and no fractional scaling. An 8x8 glyph lands 32 screen pixels
 * tall, matching the on-screen text size LuaGB/LuaMD/LuaPSX use.
 *
 * The game still goes through blit_scale() with GBA_W/GBA_H/SCALE above. Only
 * the menu uses these. */
#define UI_W      480
#define UI_H      270
#define UI_SCALE  4                       /* 480*4 = 1920, 270*4 = 1080 */

/* Audio. sceAudioOut is opened at 48kHz; the mGBA GBA core renders at
   32768Hz, so gba_glue.c resamples. SAMPLES_PER_BUF is the
   sceAudioOutOutput grain. */
#define SAMPLE_RATE      48000
#define SAMPLES_PER_BUF  256
#define AUDIO_S16_STEREO 1

/* mGBA GBA audio native rate (GBA hardware clock / timer prescale). */
#define GBA_APU_RATE 32768

/* Saves live in the game's savedata and nowhere else: the savedata image is
 * the unit that gets backed up, resigned and reasoned about.
 *
 * ROMs are read from ROM_DIR first, then from TEMP_ROM_DIR. The second path
 * is the OFW story: with no save manager on OFW, ROMs cannot be injected
 * into the container, so they are uploaded over the network into /temp0 --
 * same tradeoff as LuaPSX's /temp0 discs. /temp0 survives a game relaunch
 * but NOT a console reboot, so uploads are re-sent after one. */
#define ROM_DIR      "/savedata0/roms/"
#define SAVE_DIR     "/savedata0/saves/"
#define TEMP_ROM_DIR "/temp0/roms/"

/* Calls a native function through the host eboot's argument-shuffling gadget.
   rdi = gadget, rsi = target fn, then the six real arguments. The gadget
   expects the callee in rbx and the first argument already in rdi. */
__attribute__((naked))
static u64 native_call(void *gadget, void *fn,
                       u64 a1, u64 a2, u64 a3,
                       u64 a4, u64 a5, u64 a6)
{
    __asm__ volatile (
        "pushq %%rbx\n\t"
        "movq %%rsi, %%rbx\n\t"
        "movq %%rdi, %%rax\n\t"
        "movq %%rdx, %%rdi\n\t"
        "movq %%rcx, %%rsi\n\t"
        "movq %%r8,  %%rdx\n\t"
        "movq %%r9,  %%rcx\n\t"
        "movq 16(%%rsp), %%r8\n\t"
        "movq 24(%%rsp), %%r9\n\t"
        "callq *%%rax\n\t"
        "popq %%rbx\n\t"
        "retq" ::: "memory"
    );
}

static void *resolve_sym(void *gadget, void *dlsym_fn, s32 handle, const char *name) {
    void *addr = 0;
    native_call(gadget, dlsym_fn, (u64)handle, (u64)name, (u64)&addr, 0, 0, 0);
    return addr;
}

#define NC  native_call
#define SYM resolve_sym

/* Arguments handed in by the Lua launcher. Layout is mirrored by
 * lua/gba.lua.in -- keep the two in sync. */
struct ext_args {
    s64 status;
    s64 step;
    u32 frame_count;
    u32 _pad;
    s32 log_fd;
    s32 pad_fd;
    u8  log_addr[16];
    u64 dbg[8];
};

/* One entry in the ROM picker. */
#define MAX_ROMS 4096
#define MAX_NAME 28

struct rom_entry {
    char filename[128];   /* full path -- ROMs come from more than one dir */
    char display[MAX_NAME];
};

#endif
