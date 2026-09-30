#!/usr/bin/env python3
"""Upload a GBA ROM to a running GbaC0re payload over HTTP.

    python3 upload_rom.py <PS5_IP> <rom.gba>

The payload serves its controller page on port 9030; POST /rom?name=<file>
streams the ROM into /temp0/roms/ on the console. /temp0 is wiped on reboot,
so re-upload after one -- cartridge saves are unaffected (the payload writes
/savedata0/saves itself, no resigning needed).

OFW-friendly: no save manager, no USB decrypt/resign dance per ROM.
Stdlib only.
"""
import http.client
import os
import re
import sys
import urllib.parse

MAX_ROM = 32 * 1024 * 1024

# Must match the payload's upload_name_ok(): [A-Za-z0-9._-], 5..64 chars,
# no leading dot, no "..", must end .gba (case-insensitive).
NAME_RE = re.compile(r'^[A-Za-z0-9._-]{1,64}$')


def name_ok(name):
    if not NAME_RE.match(name):
        return False
    if name[0] == '.':
        return False
    if '..' in name:
        return False
    return name.lower().endswith('.gba') and len(name) >= 5


def roms_begin(ip, count):
    """Tell the payload a batch of `count` ROMs is coming.

    The payload shows a progress bar and opens the ROM picker only after
    all `count` uploads complete. Best-effort: failures are ignored so a
    manual single upload still works.
    """
    try:
        conn = http.client.HTTPConnection(ip, 9030, timeout=10)
        conn.request("POST", "/roms_begin?count=%d" % count)
        r = conn.getresponse()
        r.read()
        conn.close()
    except Exception:
        pass


def upload_rom(ip, path):
    """POST a .gba file to the payload's /rom endpoint.

    Returns the uploaded filename on success; raises SystemExit with a
    clear message on rejection or connection failure.
    """
    if not os.path.isfile(path):
        raise SystemExit("no such file: %s" % path)
    size = os.path.getsize(path)
    if size < 1 or size > MAX_ROM:
        raise SystemExit("ROM must be 1 byte..32MB, got %d bytes" % size)

    name = os.path.basename(path)
    if not name_ok(name):
        raise SystemExit(
            "bad filename %r: payload requires [A-Za-z0-9._-], 5..64 chars, "
            "no spaces, must end .gba -- rename the file and retry" % name)

    url = "/rom?name=" + urllib.parse.quote(name, safe="")

    try:
        conn = http.client.HTTPConnection(ip, 9030, timeout=30)
        conn.putrequest("POST", url)
        conn.putheader("Content-Length", str(size))
        conn.putheader("Content-Type", "application/octet-stream")
        conn.endheaders()

        sent = 0
        with open(path, "rb") as f:
            while True:
                chunk = f.read(65536)
                if not chunk:
                    break
                conn.send(chunk)
                sent += len(chunk)
                print("\r%s: %d/%d bytes (%.0f%%)" % (name, sent, size, 100.0 * sent / size),
                      end="", flush=True)
        print()

        resp = conn.getresponse()
        resp.read()
        conn.close()
    except (OSError, http.client.HTTPException) as e:
        raise SystemExit("connection failed: %s\n"
                         "Is the payload running and is %s reachable on port 9030?"
                         % (e, ip))

    if resp.status in (200, 204):
        return name
    elif resp.status == 400:
        raise SystemExit("rejected: bad filename or size "
                         "(names: [A-Za-z0-9._-], <=64 chars, must end .gba; max 32MB)")
    elif resp.status == 409:
        raise SystemExit("rejected: another upload is already in progress")
    else:
        raise SystemExit("rejected: HTTP %d %s" % (resp.status, resp.reason))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    name = upload_rom(sys.argv[1], sys.argv[2])
    print("OK: %s uploaded -- open the picker (MENU) to refresh" % name)


if __name__ == "__main__":
    main()
