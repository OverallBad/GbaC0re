#!/usr/bin/env python3
"""GbaC0re build prerequisites checker.

Verifies every tool, source file, and dependency needed by build.py
*before* attempting a build, so failures are caught early with a clear
message instead of a cryptic compiler error halfway through.

Usage:
    python3 check_requirements.py          # check only
    python3 check_requirements.py --fix    # check + auto-clone mGBA if missing

Exit code 0 = all good, non-zero = something is missing.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))

# --- What we check -----------------------------------------------------------

# External tools (must be on PATH)
REQUIRED_TOOLS = ["gcc", "objcopy", "readelf", "objdump", "ar"]

# mGBA source tree (not shipped; must be cloned)
MGBA_SRC = os.environ.get("MGBA_SRC", os.path.join(ROOT, "mgba"))
MGBA_MARKERS = [
    os.path.join("include", "mgba", "core", "core.h"),
    os.path.join("src", "gba", "gba.h"),
]

# Link-cable bridge sources (shipped in link/)
LINK_DIR_CANDIDATES = [
    os.path.join(ROOT, "link"),
    os.path.join(ROOT, "..", "..", "link"),
]
LINK_FILES = ["bridge.c", "bridge.h", "link_proto.c", "link_proto.h",
              "link_ps5.c"]

# Payload source files (shipped in src/)
REQUIRED_SOURCES = [
    "src/main.c", "src/gba_glue.c", "src/gba_glue.h",
    "src/gba_runtime.c", "src/menu.c", "src/menu.h",
    "src/ui.c", "src/ui.h", "src/savestate.c", "src/savestate.h",
    "src/savedata.c", "src/savedata.h", "src/shim.c", "src/shim.h",
    "src/mgba_stubs.c",
    "src/boot.inc", "src/fault.inc",
    "linker.ld",
    "lua/gba.lua.in",
    "tools/mklua.py",
]

MGBA_CLONE_URL = "https://github.com/mgba-emu/mgba.git"


def check_tools():
    """Return list of missing tools."""
    return [t for t in REQUIRED_TOOLS if shutil.which(t) is None]


def check_mgba():
    """Return True if the mGBA source tree looks valid."""
    for marker in MGBA_MARKERS:
        if not os.path.isfile(os.path.join(MGBA_SRC, marker)):
            return False
    return True


def check_link():
    """Return (link_dir, missing_files) or (None, LINK_FILES)."""
    for candidate in LINK_DIR_CANDIDATES:
        if os.path.isdir(candidate):
            missing = [f for f in LINK_FILES
                       if not os.path.isfile(os.path.join(candidate, f))]
            return candidate, missing
    return None, LINK_FILES


def check_sources():
    """Return list of missing source files."""
    return [s for s in REQUIRED_SOURCES
            if not os.path.isfile(os.path.join(ROOT, s))]


def clone_mgba():
    """Attempt to clone the mGBA source tree."""
    print(f"  Cloning mGBA from {MGBA_CLONE_URL} ...")
    print(f"  Destination: {MGBA_SRC}")
    try:
        subprocess.run(
            ["git", "clone", "--depth", "1", MGBA_CLONE_URL, MGBA_SRC],
            check=True)
        return True
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        print(f"  Clone failed: {e}")
        return False


def main():
    auto_fix = "--fix" in sys.argv
    failures = 0

    print("GbaC0re build prerequisites check")
    print("=" * 40)

    # 1. Python version
    print(f"\n[1/5] Python {sys.version_info.major}.{sys.version_info.minor} ... ", end="")
    if sys.version_info >= (3, 8):
        print("OK")
    else:
        print("FAIL (need 3.8+)")
        failures += 1

    # 2. Build tools
    print("[2/5] Build tools ... ", end="")
    missing_tools = check_tools()
    if not missing_tools:
        print("OK (gcc, objcopy, readelf, objdump, ar)")
    else:
        print(f"FAIL — missing: {', '.join(missing_tools)}")
        print("  Install: apt install build-essential binutils")
        print("           (Windows: install MinGW-w64)")
        failures += 1

    # 3. mGBA source tree
    print("[3/5] mGBA source tree ... ", end="")
    if check_mgba():
        print(f"OK ({MGBA_SRC})")
    else:
        print(f"MISSING ({MGBA_SRC})")
        if auto_fix:
            if clone_mgba() and check_mgba():
                print("  Clone successful.")
            else:
                print("  Clone failed.")
                failures += 1
        else:
            print(f"  Fetch it with:")
            print(f"    git clone --depth 1 {MGBA_CLONE_URL} \"{MGBA_SRC}\"")
            print(f"  Or run: python3 check_requirements.py --fix")
            print(f"  (mGBA is MPL-2.0, not shipped with GbaC0re)")
            failures += 1

    # 4. Link-cable bridge sources
    print("[4/5] Link-cable bridge ... ", end="")
    link_dir, missing_link = check_link()
    if link_dir and not missing_link:
        print(f"OK ({link_dir})")
    else:
        print(f"FAIL — missing: {', '.join(missing_link)}")
        failures += 1

    # 5. Payload sources
    print("[5/5] Payload sources ... ", end="")
    missing_src = check_sources()
    if not missing_src:
        print(f"OK ({len(REQUIRED_SOURCES)} files)")
    else:
        print(f"FAIL — missing {len(missing_src)} file(s):")
        for f in missing_src:
            print(f"    {f}")
        failures += 1

    # Summary
    print("\n" + "=" * 40)
    if failures == 0:
        print("All prerequisites met. Run: python3 build.py")
        return 0
    else:
        print(f"{failures} check(s) FAILED. Fix the above, then re-run.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
