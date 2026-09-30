#!/usr/bin/env python3
"""GbaC0re v0.7 launcher -- sends the payload to the Luac0re loader.

    python gba_launcher.py <PS5_IP>
    python gba_launcher.py <PS5_IP> --log
    python gba_launcher.py <PS5_IP> --payload lua/gba.lua --blob gba_emu.bin

Two stages. Luac0re caps a script at 500KB and hex encoding doubles whatever
is embedded, so the emulator binary cannot ride inside the Lua the way the
smaller cores do. The script binds a TCP port and this streams the binary to
it.

ROMs are uploaded separately: put .gba files in /savedata0/roms/ on the
console (Luac0re file browser), or see README_GBA.md. Battery saves land in
/savedata0/saves/<game>.sav.

Before sending, arm the loader: launch the host game and open
OPTIONS -> HALL OF FAME.
"""

import argparse
import glob
import http.client
import os
import socket
import sys
import threading
import time

LOADER_PORT = 9026
LOG_PORT = 9027
BLOB_PORT_LO, BLOB_PORT_HI = 9028, 9045
WEB_PORT = 9030


def wait_for_web(ip, timeout=30):
    """Poll the payload's controller page until it answers or timeout hits."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            conn = http.client.HTTPConnection(ip, WEB_PORT, timeout=3)
            conn.request("GET", "/")
            resp = conn.getresponse()
            resp.read()
            conn.close()
            if resp.status < 500:
                return True
        except (OSError, http.client.HTTPException):
            pass
        time.sleep(1)
    return False


def auto_upload_roms(ip, romdir):
    """Upload every .gba in romdir to the payload, one at a time."""
    from upload_rom import upload_rom, name_ok, roms_begin
    # Dedupe: on Windows the FS is case-insensitive, so *.gba already
    # matches *.GBA -- a set keeps each file once.
    seen = set()
    roms = []
    for pat in (os.path.join(romdir, "*.gba"), os.path.join(romdir, "*.GBA")):
        for p in sorted(glob.glob(pat)):
            key = os.path.normcase(os.path.abspath(p))
            if key not in seen:
                seen.add(key)
                roms.append(p)
    roms.sort()
    if not roms:
        print("no .gba files in %s -- skipping auto-upload" % romdir)
        return
    print("waiting for payload web server on :%d ..." % WEB_PORT)
    if not wait_for_web(ip):
        print("web server never came up -- skipping auto-upload")
        return
    # Announce the batch so the payload shows a progress bar and waits
    # for all ROMs before opening the picker.
    roms_begin(ip, len(roms))
    for path in roms:
        base = os.path.basename(path)
        if not name_ok(base):
            print("SKIP %s: name not allowed by payload "
                  "([A-Za-z0-9._-], <=64 chars, .gba) -- rename to upload" % base)
            continue
        ok = False
        for attempt in (1, 2):
            try:
                name = upload_rom(ip, path)
                print("OK: %s uploaded" % name)
                ok = True
                break
            except SystemExit as e:
                msg = str(e)
                if "connection failed" in msg and attempt == 1:
                    print("retry %s after connection abort ..." % base)
                    time.sleep(2)
                    continue
                print("FAILED %s: %s" % (base, msg))
                break
        time.sleep(0.5)


def log_listener(stop):
    """The payload logs to the subnet broadcast, so nothing has to be told
    this PC's address. Binding 0.0.0.0 receives it."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind(("0.0.0.0", LOG_PORT))
    except OSError as e:
        print("log: cannot bind udp/%d (%s)" % (LOG_PORT, e))
        return
    s.settimeout(1.0)
    while not stop.is_set():
        try:
            data, _ = s.recvfrom(4096)
        except socket.timeout:
            continue
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()


def send_script(host, path):
    data = open(path, "rb").read()
    s = socket.create_connection((host, LOADER_PORT), timeout=20)
    try:
        s.sendall(data)
        s.shutdown(socket.SHUT_WR)
    finally:
        s.close()
    print("script: %d bytes -> %s:%d" % (len(data), host, LOADER_PORT))


def send_blob(host, path, tries=40):
    """Find the live receiver by its ACK, then stream.

    A listener left behind by a dead script still accepts and buffers, so a
    successful connect proves nothing -- it only resets once its buffer fills
    part way through. The ACK byte is what distinguishes them.
    """
    data = open(path, "rb").read()
    for _ in range(tries):
        for port in range(BLOB_PORT_LO, BLOB_PORT_HI + 1):
            try:
                s = socket.create_connection((host, port), timeout=5)
            except OSError:
                continue
            try:
                s.settimeout(5)
                if s.recv(1) != b"K":
                    raise OSError("no ACK")
                s.settimeout(300)
                s.sendall(data)
                s.shutdown(socket.SHUT_WR)
            except OSError:
                s.close()
                continue
            s.close()
            print("blob  : %d bytes -> %s:%d" % (len(data), host, port))
            return True
        time.sleep(0.25)
    print("blob  : no live receiver on %d-%d" % (BLOB_PORT_LO, BLOB_PORT_HI))
    return False


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("ip")
    ap.add_argument("--payload", default=os.path.join(here, "lua", "gba.lua"))
    ap.add_argument("--blob", default=os.path.join(here, "gba_emu.bin"))
    ap.add_argument("--log", action="store_true",
                    help="print the payload's UDP log on port 9027")
    ap.add_argument("--roms", metavar="DIR", default=None,
                    help="auto-upload every .gba in DIR to /temp0/roms/ after launch")
    a = ap.parse_args()

    for f in (a.payload, a.blob):
        if not os.path.isfile(f):
            raise SystemExit("missing %s -- run `make` first" % f)
    if a.roms and not os.path.isdir(a.roms):
        raise SystemExit("not a directory: %s" % a.roms)

    stop = threading.Event()
    if a.log:
        # Started BEFORE the send: the first lines arrive within milliseconds
        # and a listener opened afterwards loses the bring-up.
        threading.Thread(target=log_listener, args=(stop,), daemon=True).start()
        time.sleep(0.3)

    try:
        send_script(a.ip, a.payload)
        if not send_blob(a.ip, a.blob):
            return 1
        print("sent. payload should be running.")
        if a.roms:
            auto_upload_roms(a.ip, a.roms)
        if a.log:
            print("--- log (ctrl-c to stop) ---")
            while True:
                time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
    return 0


if __name__ == "__main__":
    sys.exit(main())
