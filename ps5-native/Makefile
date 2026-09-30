# GbaC0re PS5 native v2.0.0 — native-title Makefile
#
# Builds a real PS5 homebrew title (not a payload):
#   1. prospero-clang (ps5-payload-sdk v0.43) compiles each TU to an object
#   2. ps5link's link_real links objects + crt1_ps5.o into app.elf
#      (SCE dynamic ELF, imports by NID)
#   3. signing to eboot.bin needs SharpProspero + .NET (see INSTALL.md);
#      without it, this Makefile stops at app.elf
#
# Usage:
#   make                    # app.elf
#   make package            # package/ dir layout (needs eboot.bin, see below)
#
# Env:
#   PS5_PAYLOAD_SDK  ps5-payload-sdk install (default $(HOME)/ps5-payload-sdk)
#   PS5LINK          ps5link-sdk checkout (default vendor/ps5link-sdk)

PS5_PAYLOAD_SDK ?= $(HOME)/ps5-payload-sdk
PS5LINK         ?= vendor/ps5link-sdk

PS5_CC  := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
LINKER  := $(PS5LINK)/linker/link_real
CRT1    := $(PS5LINK)/linker/crt1_ps5.o

MGBA_SRC ?= ../work/v07/mgba-test

BUILD := build

# mGBA core sources (same known-good list as the PC port).
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
    $(MGBA_SRC)/src/util/vector.c \
    $(MGBA_SRC)/src/util/vfs.c \
    $(MGBA_SRC)/src/util/vfs/vfs-fifo.c \
    $(MGBA_SRC)/src/util/vfs/vfs-mem.c

# GbaC0re native sources. NOTE: link_ps5.c is the old LuaC0re gadget backend
# and is deliberately excluded; link_proto.c uses its POSIX socket backend.
APP_SRCS = \
    src/ps5_main.c \
    src/ps5_gba.c \
    src/ps5_platform_sce.c \
    src/ps5_tilemap.c \
    src/ps5_diag.c \
    src/ps5_locale_shim.c \
    src/menu.c \
    src/ui.c \
    src/savestate.c \
    src/mgba_stubs.c \
    src/link_proto.c \
    src/bridge.c

OBJS = $(patsubst %.c,$(BUILD)/%.o,$(notdir $(APP_SRCS))) \
       $(patsubst $(MGBA_SRC)/%.c,$(BUILD)/mgba_%.o,$(MGBA_SRCS))

# -ffunction-sections/-fdata-sections + --gc-sections keep the binary lean
# (same trick that got the LuaC0re blob under the JIT ceiling).
CFLAGS = -O2 -Wall \
         -include stdio.h -include stdint.h -include ps5_libc_fixups.h \
         -I$(MGBA_SRC)/include \
         -I$(MGBA_SRC)/src \
         -Isrc \
         -DMGBA_CORE_CHEATS=0 \
         -DM_CORE_GBA=1 \
         -DLINK_PS5_NATIVE \
         -DHAVE_LOCALE -DHAVE_STRTOF_L \
         -ffunction-sections -fdata-sections

all: app.elf

$(BUILD):
	mkdir -p $(BUILD)

# One rule per source dir (make has no per-file mapping here; explicit).
$(BUILD)/%.o: src/%.c | $(BUILD)
	$(PS5_CC) $(CFLAGS) -c $< -o $@

$(BUILD)/mgba_%.o: $(MGBA_SRC)/%.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(PS5_CC) $(CFLAGS) -c $< -o $@

$(CRT1): $(PS5LINK)/linker/crt1.S
	$(PS5_CC) -c $< -o $@

app.elf: $(OBJS) $(CRT1) $(LINKER)
	$(LINKER) $@ $(CRT1) $(OBJS)

$(LINKER):
	$(MAKE) -C $(PS5LINK) linker/link_real

clean:
	rm -rf $(BUILD) app.elf

.PHONY: all clean
