# GbaC0re PC — Makefile
#
# Builds the SDL2 PC port. Requires:
#   - SDL2 dev headers (libsdl2-dev)
#   - mGBA source at ../work/v07/mgba-test (or set MGBA_SRC)
#
# Usage:
#   make
#   ./gbac0re_pc <rom.gba>

MGBA_SRC ?= ../work/v07/mgba-test
LINK_SRC ?= ../link
CC = gcc
CFLAGS = -O2 -Wall -Wno-unused-function \
         -include stdio.h \
         -I$(MGBA_SRC)/include \
         -I$(MGBA_SRC)/src \
         -I$(LINK_SRC) \
         -Isrc \
         $(shell sdl2-config --cflags 2>/dev/null || echo "-I/usr/include/SDL2") \
         -DMGBA_CORE_CHEATS=0 \
         -DM_CORE_GBA=1 \
         $(if $(findstring MINGW,$(shell uname -s)),,-DHAVE_LOCALE -DHAVE_STRTOF_L)
# HAVE_LOCALE is omitted on MinGW: its headers name the type _locale_t, not
# locale_t, and mGBA's formatting.h expects the POSIX name.

ifeq ($(STATIC),1)
# Self-contained: static SDL2 + static libgcc/libstdc++.
LDFLAGS = $(shell sdl2-config --static-libs 2>/dev/null || echo "-static -lSDL2") -lm -static-libgcc
else
LDFLAGS = $(shell sdl2-config --libs 2>/dev/null || echo "-lSDL2") -lm
endif
# -lshlwapi: mGBA's vfs.c calls PathIsRelativeW (Windows shell API).
# -lcomdlg32: GetOpenFileName for the File > Open ROM dialog.
# Winsock for the link-cable bridge (link_proto.c).
ifneq (,$(findstring MINGW,$(shell uname -s 2>/dev/null)))
LDFLAGS += -lshlwapi -lcomdlg32 -lws2_32
endif
ifeq ($(OS),Windows_NT)
LDFLAGS += -lshlwapi -lcomdlg32 -lws2_32
endif
#
# Static build (self-contained .exe, no SDL2.dll needed):
#   make STATIC=1
# Uses sdl2-config --static-libs for the full Windows system lib set.

# mGBA sources (same set as the PS5 build, minus cheat engine which is stubbed).
MGBA_SRCS = \
    $(MGBA_SRC)/src/arm/arm.c \
    $(MGBA_SRC)/src/arm/decoder-arm.c \
    $(MGBA_SRC)/src/arm/decoder-thumb.c \
    $(MGBA_SRC)/src/arm/decoder.c \
    $(MGBA_SRC)/src/arm/isa-arm.c \
    $(MGBA_SRC)/src/arm/isa-thumb.c \
    $(MGBA_SRC)/src/core/bitmap-cache.c \
    $(MGBA_SRC)/src/core/cache-set.c \
    $(MGBA_SRC)/src/core/config.c \
    $(MGBA_SRC)/src/core/core.c \
    $(MGBA_SRC)/src/core/directories.c \
    $(MGBA_SRC)/src/core/input.c \
    $(MGBA_SRC)/src/core/interface.c \
    $(MGBA_SRC)/src/core/library.c \
    $(MGBA_SRC)/src/core/lockstep.c \
    $(MGBA_SRC)/src/core/log.c \
    $(MGBA_SRC)/src/core/map-cache.c \
    $(MGBA_SRC)/src/core/mem-search.c \
    $(MGBA_SRC)/src/core/rewind.c \
    $(MGBA_SRC)/src/core/serialize.c \
    $(MGBA_SRC)/src/core/sync.c \
    $(MGBA_SRC)/src/core/thread.c \
    $(MGBA_SRC)/src/core/tile-cache.c \
    $(MGBA_SRC)/src/core/timing.c \
    $(MGBA_SRC)/src/gb/audio.c \
    $(MGBA_SRC)/src/gba/audio.c \
    $(MGBA_SRC)/src/gba/bios.c \
    $(MGBA_SRC)/src/gba/cart/ereader.c \
    $(MGBA_SRC)/src/gba/cart/gpio.c \
    $(MGBA_SRC)/src/gba/cart/matrix.c \
    $(MGBA_SRC)/src/gba/cart/unlicensed.c \
    $(MGBA_SRC)/src/gba/cart/vfame.c \
    $(MGBA_SRC)/src/gba/core.c \
    $(MGBA_SRC)/src/gba/dma.c \
    $(MGBA_SRC)/src/gba/extra/battlechip.c \
    $(MGBA_SRC)/src/gba/extra/proxy.c \
    $(MGBA_SRC)/src/gba/gba.c \
    $(MGBA_SRC)/src/gba/hle-bios.c \
    $(MGBA_SRC)/src/gba/input.c \
    $(MGBA_SRC)/src/gba/io.c \
    $(MGBA_SRC)/src/gba/memory.c \
    $(MGBA_SRC)/src/gba/overrides.c \
    $(MGBA_SRC)/src/gba/renderers/cache-set.c \
    $(MGBA_SRC)/src/gba/renderers/common.c \
    $(MGBA_SRC)/src/gba/renderers/gl.c \
    $(MGBA_SRC)/src/gba/renderers/software-bg.c \
    $(MGBA_SRC)/src/gba/renderers/software-mode0.c \
    $(MGBA_SRC)/src/gba/renderers/software-obj.c \
    $(MGBA_SRC)/src/gba/renderers/video-software.c \
    $(MGBA_SRC)/src/gba/savedata.c \
    $(MGBA_SRC)/src/gba/serialize.c \
    $(MGBA_SRC)/src/gba/sharkport.c \
    $(MGBA_SRC)/src/gba/sio.c \
    $(MGBA_SRC)/src/gba/sio/gbp.c \
    $(MGBA_SRC)/src/gba/timer.c \
    $(MGBA_SRC)/src/gba/video.c \
    $(MGBA_SRC)/src/feature/thread-proxy.c \
    $(MGBA_SRC)/src/feature/video-logger.c \
    $(MGBA_SRC)/src/third-party/inih/ini.c \
    $(MGBA_SRC)/src/util/audio-buffer.c \
    $(MGBA_SRC)/src/util/audio-resampler.c \
    $(MGBA_SRC)/src/util/circle-buffer.c \
    $(MGBA_SRC)/src/util/configuration.c \
    $(MGBA_SRC)/src/util/convolve.c \
    $(MGBA_SRC)/src/util/crc32.c \
    $(MGBA_SRC)/src/util/elf-read.c \
    $(MGBA_SRC)/src/util/formatting.c \
    $(MGBA_SRC)/src/util/gbk-table.c \
    $(MGBA_SRC)/src/util/geometry.c \
    $(MGBA_SRC)/src/util/hash.c \
    $(MGBA_SRC)/src/util/image.c \
    $(MGBA_SRC)/src/util/image/export.c \
    $(MGBA_SRC)/src/util/image/font.c \
    $(MGBA_SRC)/src/util/image/png-io.c \
    $(MGBA_SRC)/src/util/interpolator.c \
    $(MGBA_SRC)/src/util/md5.c \
    $(MGBA_SRC)/src/util/memory.c \
    $(MGBA_SRC)/src/util/patch-fast.c \
    $(MGBA_SRC)/src/util/patch-ips.c \
    $(MGBA_SRC)/src/util/patch-ups.c \
    $(MGBA_SRC)/src/util/patch.c \
    $(MGBA_SRC)/src/util/ring-fifo.c \
    $(MGBA_SRC)/src/util/sha1.c \
    $(MGBA_SRC)/src/util/string.c \
    $(MGBA_SRC)/src/util/table.c \
    $(MGBA_SRC)/src/util/text-codec.c \
    $(MGBA_SRC)/src/util/vector.c \
    $(MGBA_SRC)/src/util/vfs.c \
    $(MGBA_SRC)/src/util/vfs/vfs-fifo.c \
    $(MGBA_SRC)/src/util/vfs/vfs-mem.c \


# PC port sources.
PC_SRCS = \
    src/pc_main.c \
    src/pc_gba.c \
    src/menu.c \
    src/ui.c \
    src/savestate.c \
    src/pc_menu_bar.c \
    src/mgba_stubs.c

# Link-cable bridge sources (shared with the PS5 payload).
LINK_BRIDGE_SRCS = \
    $(LINK_SRC)/link_proto.c \
    $(LINK_SRC)/bridge.c

OBJS = $(MGBA_SRCS:.c=.o) $(PC_SRCS:.c=.o) $(LINK_BRIDGE_SRCS:.c=.o)

TARGET = gbac0re_pc
# .exe suffix on Windows (MSYS2/MinGW).
ifeq ($(OS),Windows_NT)
TARGET := $(TARGET).exe
endif
ifneq (,$(findstring MINGW,$(shell uname -s 2>/dev/null)))
TARGET := gbac0re_pc.exe
endif

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)
	@echo "Built: $(TARGET)"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
