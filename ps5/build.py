#!/usr/bin/env python3
"""GbaC0re v0.7 -- build driver. Replaces the Makefile; needs no `make`.

    python3 build.py           compile mGBA + runtime, link, emit gba_emu.bin
                               and fold it into lua/gba.lua
    python3 build.py check     build, then run the position-independence
                               checks (relocations + image integrity)
    python3 build.py clean     remove build products (keeps build/mgba cache)
    python3 build.py distclean remove everything including the object cache

Requires: gcc, objcopy, readelf, objdump, ar, python3 -- and an mGBA source
tree (not shipped). Fetch it once:

    git clone https://github.com/mgba-emu/mgba.git mgba

Environment overrides (same names as the Makefile): CC, OBJCOPY, READELF,
OBJDUMP, AR, MGBA_SRC.
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))

CC      = os.environ.get("CC", "gcc")
OBJCOPY = os.environ.get("OBJCOPY", "objcopy")
READELF = os.environ.get("READELF", "readelf")
OBJDUMP = os.environ.get("OBJDUMP", "objdump")
AR      = os.environ.get("AR", "ar")
MGBA_SRC = os.environ.get("MGBA_SRC", os.path.join(ROOT, "mgba"))

TARGET = "gba_emu"

# Link-cable bridge sources (shared with the PC port).
LINK_DIR = os.path.join(ROOT, "..", "..", "link")

BASEFLAGS = ["-Os", "-ffreestanding", "-fno-stack-protector", "-fno-builtin",
             "-fpie", "-mno-red-zone", "-fomit-frame-pointer",
             "-fcf-protection=none", "-fno-exceptions", "-fno-unwind-tables",
             "-fno-asynchronous-unwind-tables", "-fno-strict-aliasing",
             "-fvisibility=hidden",
             "-ffunction-sections", "-fdata-sections"]

MGBA_DEFINES = ["-DBUILD_STATIC", "-DENABLE_DIRECTORIES", "-DENABLE_VFS",
                "-DHAVE_FUTIMENS", "-DHAVE_FUTIMES",
                "-DMINIMAL_CORE=1", "-DM_CORE_GBA=1", "-DDISABLE_THREADING=1",
                "-DNDEBUG", "-DPATH_MAX=4096", "-DENABLE_VFS_FILE=1"]

MGBA_CFLAGS = (BASEFLAGS + ["-std=c11", "-fwrapv", "-w", "-U_FORTIFY_SOURCE",
                            "-Isrc/libc", "-Isrc",
                            "-I" + os.path.join(MGBA_SRC, "include"),
                            "-I" + os.path.join(MGBA_SRC, "src")] + MGBA_DEFINES)

SRCFLAGS = BASEFLAGS + ["-Isrc/libc", "-Isrc", "-Wall", "-Wno-unused-function",
                        "-I" + os.path.join(MGBA_SRC, "include"),
                        "-I" + LINK_DIR, "-DLINK_PS5"] + MGBA_DEFINES

# -static-pie, NOT "-static -pie": with plain -static the linker emits ET_EXEC
# and relaxes RIP-relative GOT loads into absolute immediates, silently
# destroying position independence for cross-TU globals. -static-pie emits
# ET_DYN; the R_X86_64_RELATIVE relocs it keeps are applied by _start at boot.
LDFLAGS = ["-T", "linker.ld", "-nostdlib", "-nostartfiles", "-static-pie",
           "-Wl,--build-id=none", "-Wl,-z,norelro", "-Wl,--gc-sections"]

MGBA_SRCS = """\
src/arm/arm.c
src/arm/decoder-arm.c
src/arm/decoder-thumb.c
src/arm/decoder.c
src/arm/isa-arm.c
src/arm/isa-thumb.c
src/core/bitmap-cache.c
src/core/cache-set.c
src/core/config.c
src/core/core.c
src/core/directories.c
src/core/input.c
src/core/interface.c
src/core/library.c
src/core/lockstep.c
src/core/log.c
src/core/map-cache.c
src/core/mem-search.c
src/core/rewind.c
src/core/serialize.c
src/core/sync.c
src/core/thread.c
src/core/tile-cache.c
src/core/timing.c
src/gb/audio.c
src/gba/audio.c
src/gba/bios.c
src/gba/cart/ereader.c
src/gba/cart/gpio.c
src/gba/cart/matrix.c
src/gba/cart/unlicensed.c
src/gba/cart/vfame.c
src/gba/core.c
src/gba/dma.c
src/gba/extra/battlechip.c
src/gba/extra/proxy.c
src/gba/gba.c
src/gba/hle-bios.c
src/gba/input.c
src/gba/io.c
src/gba/memory.c
src/gba/overrides.c
src/gba/renderers/cache-set.c
src/gba/renderers/common.c
src/gba/renderers/gl.c
src/gba/renderers/software-bg.c
src/gba/renderers/software-mode0.c
src/gba/renderers/software-obj.c
src/gba/renderers/video-software.c
src/gba/savedata.c
src/gba/serialize.c
src/gba/sharkport.c
src/gba/sio.c
src/gba/sio/gbp.c
src/gba/timer.c
src/gba/video.c
src/third-party/inih/ini.c
src/util/audio-buffer.c
src/util/audio-resampler.c
src/util/circle-buffer.c
src/util/configuration.c
src/util/convolve.c
src/util/crc32.c
src/util/elf-read.c
src/util/formatting.c
src/util/gbk-table.c
src/util/geometry.c
src/util/hash.c
src/util/image.c
src/util/image/export.c
src/util/image/font.c
src/util/image/png-io.c
src/util/interpolator.c
src/util/md5.c
src/util/memory.c
src/util/patch-fast.c
src/util/patch-ips.c
src/util/patch-ups.c
src/util/patch.c
src/util/ring-fifo.c
src/util/sha1.c
src/util/string.c
src/util/table.c
src/util/text-codec.c
src/util/vector.c
src/util/vfs.c
src/util/vfs/vfs-fifo.c
src/util/vfs/vfs-mem.c
src/util/vfs/vfs-file.c
""".split()

SRC_SRCS = ["src/main.c", "src/gba_glue.c", "src/gba_runtime.c",
            "src/shim.c", "src/ui.c", "src/menu.c", "src/savedata.c", "src/mgba_stubs.c",
            "src/savestate.c",
            os.path.join("..", "..", "link", "link_proto.c"),
            os.path.join("..", "..", "link", "link_ps5.c"),
            os.path.join("..", "..", "link", "bridge.c")]

HEADERS = ["src/core.h", "src/shim.h", "src/gba_glue.h",
           "src/ui.h", "src/menu.h", "src/tables.h", "src/savedata.h"]


def run(cmd, **kw):
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True, cwd=ROOT, **kw)


def newer(src, dst):
    return (not os.path.exists(dst)
            or os.path.getmtime(src) > os.path.getmtime(dst))


def check_mgba():
    cmake = os.path.join(MGBA_SRC, "CMakeLists.txt")
    if not os.path.isfile(cmake):
        sys.exit(
            "mGBA source not found at %s.\n"
            "Fetch it first: git clone https://github.com/mgba-emu/mgba.git %s"
            % (MGBA_SRC, MGBA_SRC))


def build_mgba():
    check_mgba()
    objs = []
    for rel in MGBA_SRCS:
        src = os.path.join(MGBA_SRC, rel)
        obj = os.path.join(ROOT, "build", "mgba",
                           os.path.splitext(rel)[0] + ".o")
        objs.append(obj)
        if newer(src, obj):
            os.makedirs(os.path.dirname(obj), exist_ok=True)
            run([CC] + MGBA_CFLAGS + ["-c", src, "-o", obj])
    lib = os.path.join(ROOT, "build", "libmgba.a")
    if newer(__file__, lib) or any(newer(o, lib) for o in objs):
        run([AR, "rcs", lib] + objs)
    return lib, objs


def build_src():
    objs = []
    for rel in SRC_SRCS:
        src = os.path.join(ROOT, rel)
        obj = os.path.splitext(src)[0] + ".o"
        objs.append(obj)
        stale = newer(src, obj) or any(
            newer(os.path.join(ROOT, h), obj)
            for h in HEADERS if os.path.exists(os.path.join(ROOT, h)))
        if stale:
            run([CC] + SRCFLAGS + ["-c", src, "-o", obj])
    return objs


def link(src_objs, lib):
    elf = os.path.join(ROOT, TARGET + ".elf")
    inputs = src_objs + [lib]
    if newer(__file__, elf) or any(newer(i, elf) for i in inputs):
        run([CC] + LDFLAGS + src_objs + [lib, "-o", elf])
    return elf


def to_bin(elf):
    blob = os.path.join(ROOT, TARGET + ".bin")
    if newer(elf, blob):
        run([OBJCOPY, "-O", "binary", elf, blob])
    return blob


def make_lua(blob):
    template = os.path.join(ROOT, "lua", "gba.lua.in")
    out = os.path.join(ROOT, "lua", "gba.lua")
    if newer(template, out) or newer(blob, out):
        run([sys.executable, os.path.join(ROOT, "tools", "mklua.py"),
             template, blob, out])
    return out


# --- `make check` equivalents, in pure Python (no sh needed) ---

def readelf_output(*args):
    return subprocess.run([READELF] + list(args), check=True, cwd=ROOT,
                          capture_output=True, text=True).stdout


def check_relocs(elf):
    """Every dynamic relocation must be R_X86_64_RELATIVE (applied by _start)."""
    out = readelf_output("-rW", elf)
    total = len(re.findall(r"R_X86_64_\w+", out))
    bad = sorted(set(m.group(0) for m in re.finditer(r"R_X86_64_\w+", out)
                       if m.group(0) != "R_X86_64_RELATIVE"))
    if bad:
        sys.exit("check_relocs: FAIL -- non-RELATIVE dynamic relocations: %s\n"
                 "  _start only applies R_X86_64_RELATIVE; these would never "
                 "be fixed up." % ", ".join(bad))
    print("check_relocs: %d dynamic relocations, all R_X86_64_RELATIVE "
          "(applied by _start)" % total)


def check_image(elf):
    """ELF must be ET_DYN with no absolute immediates targeting functions."""
    etype = None
    for line in readelf_output("-h", elf).splitlines():
        m = re.match(r"\s*Type:\s*(\S+)", line)
        if m:
            etype = m.group(1)
    if etype != "DYN":
        sys.exit("check_image: FAIL -- ELF type is %s, expected DYN\n"
                 "  Link with -static-pie." % etype)

    funcs = set()
    for line in readelf_output("-sW", elf).splitlines():
        p = line.split()
        # Num: Value Size Type Bind Vis Ndx Name
        if len(p) >= 8 and p[3] == "FUNC":
            # Mirror check_image.sh: leading zeros are stripped, so a zero
            # address (UND/ABS symbols, `mov $0x0` zeroing idioms) becomes ""
            # and is ignored on both sides.
            funcs.add(p[1].lstrip("0").lower())
    funcs.discard("")

    asm = subprocess.run([OBJDUMP, "-d", elf], check=True, cwd=ROOT,
                         capture_output=True, text=True).stdout
    imms = set(m.group(1).lstrip("0").lower()
               for m in re.finditer(r"mov\s+\$0x([0-9a-f]+),%r\w+", asm))
    imms.discard("")
    # Immediates below 64 KiB are data constants (loop bounds, clamps like
    # 0x7ffe), never function addresses: a real absolute-address regression
    # (e.g. LDFLAGS losing -static-pie) bakes link-time VAs across the whole
    # ~360 KiB .text and still lights up above this threshold.
    imms = set(a for a in imms if int(a, 16) >= 0x10000)
    bad = sorted(funcs & imms, key=lambda a: int(a, 16))
    if bad:
        sys.exit("check_image: FAIL -- absolute immediates target functions: "
                 + ", ".join("0x" + a for a in bad[:10]))

    got = {}
    for line in readelf_output("-sW", elf).splitlines():
        p = line.split()
        if len(p) >= 8 and p[7] in ("__got_start", "__got_end"):
            got[p[7]] = p[1]
    if "__got_start" not in got or "__got_end" not in got:
        sys.exit("check_image: FAIL -- __got_start/__got_end missing; _start "
                 "cannot relocate the GOT.")
    gs = int(got["__got_start"], 16)
    ge = int(got["__got_end"], 16)
    if gs % 8:
        sys.exit("check_image: FAIL -- __got_start 0x%x not 8-aligned" % gs)
    print("check_image: ET_DYN, no absolute function immediates; "
          "GOT 0x%x..0x%x (%d slots, relocated at boot)"
          % (gs, ge, (ge - gs) // 8))


def do_build():
    lib, _ = build_mgba()
    src_objs = build_src()
    elf = link(src_objs, lib)
    blob = to_bin(elf)
    lua = make_lua(blob)
    print("built: %s, %s" % (os.path.relpath(blob, ROOT),
                             os.path.relpath(lua, ROOT)))
    return elf


def do_clean(dist=False):
    for rel in SRC_SRCS:
        obj = os.path.join(ROOT, os.path.splitext(rel)[0] + ".o")
        if os.path.exists(obj):
            os.remove(obj)
    for name in (TARGET + ".elf", TARGET + ".bin",
                 os.path.join("lua", "gba.lua")):
        p = os.path.join(ROOT, name)
        if os.path.exists(p):
            os.remove(p)
    if dist:
        shutil.rmtree(os.path.join(ROOT, "build"), ignore_errors=True)
    print("clean done")


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "build"
    if cmd == "build":
        do_build()
    elif cmd == "check":
        elf = do_build()
        check_relocs(elf)
        check_image(elf)
    elif cmd == "clean":
        do_clean()
    elif cmd == "distclean":
        do_clean(dist=True)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
