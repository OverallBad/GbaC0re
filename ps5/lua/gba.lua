-- GbaC0re v0.7 -- Game Boy Advance emulator for PS5 via Luac0re.
--
-- Emulation core: mGBA (mCore), Copyright (c) Jeffrey Pfau and contributors,
-- MPL-2.0. https://mgba.io
-- PS5 runtime pattern: LuaPSX by soniciso1 (GPL-2.0-or-later), derived from
-- EmuC0re by egycnq / EgyDevTeam.
-- Delivery: Luac0re by Gezine.
--
-- Generated from lua/gba.lua.in by tools/mklua.py -- do not edit lua/gba.lua.

-- Where the UDP debug log is sent. Listen with:  nc -u -l -p 9027
--
-- "auto" derives the destination from the console's own address and sends to
-- the subnet broadcast, so it works on any network with nothing to configure.
-- A hardcoded address here would be wrong for everybody except whoever built
-- it, and produces a silent no-log rather than an error.
--
-- Set an explicit "a.b.c.d" instead if you would rather log to one machine.
local PC_IP    = "auto"
local LOG_PORT = 9027
local LOG_BOOT = "255.255.255.255"
local WEB_PORT = 9030

local function htons(p) return ((p << 8) | (p >> 8)) & 0xFFFF end

local function inet_addr(s)
    local a, b, c, d = s:match("(%d+)%.(%d+)%.(%d+)%.(%d+)")
    return (d << 24) | (c << 16) | (b << 8) | a
end

local function make_sockaddr_in(port, ip)
    local sa = malloc(16)
    for i = 0, 15 do write8(sa + i, 0) end
    write8(sa + 0, 16)
    write8(sa + 1, 2)
    write16(sa + 2, htons(port))
    if ip then write32(sa + 4, inet_addr(ip)) end
    return sa
end

local log_sock = create_socket(AF_INET, SOCK_DGRAM, 0)
-- Sending to a broadcast address is refused with EACCES unless SO_BROADCAST is
-- set. BSD values: SOL_SOCKET 0xffff, SO_BROADCAST 0x0020 -- the PS5 kernel is
-- FreeBSD-derived, and the Linux numbers are different.
if log_sock >= 0 then
    local en = malloc(4)
    write32(en, 1)
    syscall.setsockopt(log_sock, 0xffff, 0x0020, en, 4)
end

local log_sa   = make_sockaddr_in(LOG_PORT, LOG_BOOT)

local function ulog(m)
    if log_sock >= 0 then
        syscall.sendto(log_sock, m .. "\n", #m + 1, 0, log_sa, 16)
    end
end

ulog("=== GbaC0re v1.2.15")

-- Resolve sceKernelDlsym before Luac0re's own guess is used.
--
-- init_dlsym() derives it as a fixed distance back from
-- sceKernelGetModuleInfoFromAddr and, on PS5, never checks the answer:
-- 0x450 for firmware 10.00 and up, 0x480 below that. Measured across the 55
-- retail firmwares that could be parsed, the real distance is:
--
--     0x330   02.00 - 03.21        0x480   05.00 - 09.60
--     0x340   04.00 - 04.51        0x450   10.00 - 13.42
--
-- so the built-in constant is wrong for every 2.xx, 3.xx and 4.xx console --
-- 14 firmwares. Calling that address kills the Lua VM outright, so the loader
-- prints its banner and disappears.
--
-- The address cannot be checked by reading it: PS5 system-module text is
-- execute-only, so reading libkernel faults even where calling it is fine.
-- That is why Luac0re verifies a prologue on PS4 and refuses to on PS5.
--
-- Ask the kernel instead. sys_dynlib_dlsym needs no offset and reads nothing.
-- Its number is 0x24F on all 55 firmwares, and the numbering around it is
-- frozen -- __sys_dynlib_load_prx (0x252) and __sys_randomized_path (0x25A)
-- bracket it and never move across 52 firmwares, with no __sys_* stub
-- renumbered anywhere.
--
-- Wrapped: on anything unexpected, fall through to the built-in path rather
-- than taking the loader down, which is the failure being fixed.
pcall(function()
    if type(sceKernelDlsym) == "function" then return end

    local addr = nil

    if type(syscall) == "table" and type(syscall.dlsym) == "function" then
        local out = malloc(8)
        write64(out, 0)
        if syscall.dlsym(LIBKERNEL_HANDLE, "sceKernelDlsym", out) == 0 then
            local a = read64(out)
            if a and a ~= 0 then addr = a end
        end
    end

    -- Only if the syscall is unavailable. These are measured from retail
    -- firmware, not guessed; 1.xx is left alone because its libkernel is
    -- still SELF-encrypted here and was never measured.
    if not addr then
        local major = tonumber(tostring(FW_VERSION or ""):match("^(%d+)"))
        local delta = nil
        if major then
            if major >= 10 then delta = 0x450
            elseif major >= 5 then delta = 0x480
            elseif major == 4 then delta = 0x340
            elseif major >= 2 then delta = 0x330
            end
        end
        if delta then
            addr = read64(LIBC_OFFSETS.sceKernelGetModuleInfoFromAddr) - delta
            DLSYM_FIX_MSG = string.format(
                "sys_dynlib_dlsym unavailable; using measured delta 0x%X", delta)
        end
    end

    if addr then
        SCE_KERNEL_DLSYM = addr
        sceKernelDlsym = func_wrap(addr)
        DLSYM_FIX_MSG = DLSYM_FIX_MSG
            or string.format("dlsym resolved by syscall at 0x%X", addr)
    end
end)

init_dlsym()
ulog(tostring(DLSYM_FIX_MSG or "dlsym: fix did not run (built-in path in use)"))

-- Resolve "auto" to this console's subnet broadcast address. get_current_ip()
-- is a Luac0re builtin, so the console can work this out for itself.
--
-- It is wrapped and sanity-checked: it walks a kernel interface list, and a
-- bad answer here is invisible -- "0.0.0.0" would become the destination
-- "0.0.0.255", which no one receives and which looks exactly like the payload
-- having crashed. Anything that is not a routable address falls back to the
-- all-subnets broadcast, which still reaches a listener on this segment.
if PC_IP == "auto" then
    PC_IP = "255.255.255.255"
    local ok, own = pcall(function() return tostring(get_current_ip() or "") end)
    if ok then
        local a, b, c = own:match("(%d+)%.(%d+)%.(%d+)%.%d+")
        if a and tonumber(a) > 0 and tonumber(a) ~= 127 then
            PC_IP = a .. "." .. b .. "." .. c .. ".255"
        end
        AUTO_IP_MSG = "get_current_ip -> '" .. own .. "', logging to " .. PC_IP
    else
        AUTO_IP_MSG = "get_current_ip FAILED, logging to " .. PC_IP
    end
end

local log_sa2 = make_sockaddr_in(LOG_PORT, PC_IP)
write32(log_sa + 4, read32(log_sa2 + 4))
ulog(tostring(AUTO_IP_MSG))


-- Deferred to here on purpose: it goes through dlsym, so it must not run
-- before the log exists to report a dlsym failure.
sceMsgDialogTerminate()
ulog("msgdialog terminated")

if not sceKernelLoadStartModule then
    sceKernelLoadStartModule = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelLoadStartModule"))
end

local libUser   = sceKernelLoadStartModule("libSceUserService.sprx", 0, 0, 0, 0, 0)
local getUserId = dlsym(libUser, "sceUserServiceGetInitialUser")
local uid_buf   = malloc(4)
write32(uid_buf, 0)
if getUserId then func_wrap(getUserId)(uid_buf) end
local userId = read32(uid_buf)
ulog("userId=" .. tostring(userId))

-- Web controller listener.
local web_sock = create_socket(AF_INET, 1, 0)
if web_sock >= 0 then
    local ba = make_sockaddr_in(WEB_PORT)
    local en = malloc(4)
    write32(en, 1)
    syscall.setsockopt(web_sock, SOL_SOCKET, SO_REUSEADDR, en, 4)
    syscall.bind(web_sock, ba, 16)
    syscall.listen(web_sock, 128)
    ulog("Web controller on port " .. WEB_PORT)
else
    ulog("Web socket failed")
end

-- Button mask on the wire matches GBA_BTN_* in src/gba_glue.h. Ten buttons
-- means the mask is 16 bits wide:
--   A 1  B 2  SELECT 4  START 8  RIGHT 16  LEFT 32  UP 64  DOWN 128
--   R 256  L 512
--   65534 = back to the ROM picker (saves first), 65535 = quit (hold R1,
--   or hold the EXIT pill -- both repeat the command while held)
--
-- Native DualSense mapping (src/main.c) keeps L1/R1 as the runtime's
-- menu/quit chords and puts the GBA shoulders on L2/R2, so the page mirrors
-- that: tapping L1 returns to the picker, holding R1 exits.
local html_body = [=[
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<title>GbaC0re</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent;touch-action:manipulation}
html,body{width:100%;height:100%;overflow:hidden;position:fixed}
body{background:#14101a;color:#c9a0ff;font-family:ui-monospace,Menlo,Consolas,monospace;
display:flex;align-items:center;justify-content:center;user-select:none;-webkit-user-select:none}
.shell{width:min(96vw,560px);height:min(88vh,460px);background:linear-gradient(165deg,#241f33,#14101c);
border-radius:18px 18px 60px 18px;padding:14px 18px;display:flex;flex-direction:column;
box-shadow:0 18px 50px rgba(0,0,0,.7),inset 0 1px 0 rgba(255,255,255,.06)}
.top{display:flex;justify-content:space-between;align-items:center;font-size:11px;letter-spacing:2px;color:#b088e8}
.dot{width:8px;height:8px;border-radius:50%;background:#324;border:1px solid #435}
.dot.on{background:#c9a0ff;box-shadow:0 0 8px #c9a0ff}
.shoulders{display:flex;justify-content:space-between;padding:4px 2px 0}
.sh{background:#2a2440;border:1px solid #4a3f66;border-radius:12px;padding:8px 34px;font-size:11px;
letter-spacing:2px;color:#c9a0ff}
.sh:active,.sh.hit{background:#4a3f7a}
.pad{flex:1;display:flex;align-items:center;justify-content:space-between;padding:0 6px}
.dpad{display:grid;grid-template-columns:repeat(3,46px);grid-template-rows:repeat(3,46px)}
.dpad b{background:#201c2c;border:1px solid #3d3654;border-radius:8px;display:flex;align-items:center;
justify-content:center;font-size:15px;color:#8f7fb8}
.dpad b:active,.dpad b.hit{background:#4a3f7a;color:#e8dcff}
.dpad i{visibility:hidden}
.ab{display:flex;flex-direction:column;gap:10px;align-items:center;transform:rotate(-14deg)}
.face{display:flex;gap:14px}
.rb{width:56px;height:56px;border-radius:50%;background:#5a2044;border:1px solid #7a3060;color:#f2d0e6;
display:flex;align-items:center;justify-content:center;font-size:20px;font-weight:700}
.rb:active,.rb.hit{background:#8a3068}
.btm{display:flex;gap:14px;justify-content:center;padding-top:8px}
.pill{background:#201c2c;border:1px solid #3d3654;border-radius:14px;padding:7px 16px;font-size:11px;
letter-spacing:1px;color:#b088e8}
.pill:active,.pill.hit{background:#4a3f7a;color:#e8dcff}
.sys{display:flex;gap:10px;align-items:center}
.sys .pill{border-color:#5a3a3a;color:#c98}
.upl{display:flex;gap:10px;justify-content:center;align-items:center;padding:6px 0 2px}
.upl .pill{cursor:pointer}
.ust{font-size:10px;color:#8f7fb8;letter-spacing:1px;max-width:72%;overflow:hidden;
text-overflow:ellipsis;white-space:nowrap}
</style>
</head>
<body>
<div class="shell">
  <div class="top">
    <span>GbaC0re</span>
    <span class="sys">
      <span class="pill" data-cmd="65534">MENU</span>
      <span class="pill" data-cmd="65535">EXIT</span>
      <span class="dot" id="dot"></span>
    </span>
  </div>

  <div class="shoulders">
    <span class="sh" data-bit="512">L</span>
    <span class="sh" data-bit="256">R</span>
  </div>

  <div class="pad">
    <div class="dpad">
      <i></i><b data-bit="64">&#9650;</b><i></i>
      <b data-bit="32">&#9664;</b><i></i><b data-bit="16">&#9654;</b>
      <i></i><b data-bit="128">&#9660;</b><i></i>
    </div>
    <div class="ab">
      <div class="face">
        <div class="rb" data-bit="2">B</div>
        <div class="rb" data-bit="1">A</div>
      </div>
    </div>
  </div>

  <div class="btm">
    <span class="pill" data-bit="4">SELECT</span>
    <span class="pill" data-bit="8">START</span>
  </div>

  <div class="upl">
    <label class="pill" for="romfile">&#8681; ROM</label>
    <input type="file" id="romfile" accept=".gba,.GBA" hidden>
    <span class="ust" id="ust"></span>
  </div>
</div>

<script>
var mask = 0, cmd = 0, hold_cmd = 0, dot = document.getElementById('dot'), busy = false, dirty = true;

function send() {
  if (busy || (!dirty && !hold_cmd)) return;
  busy = true; dirty = false;
  var v = cmd || hold_cmd || mask;
  cmd = 0;
  fetch('/b' + v, { method: 'POST', keepalive: true })
    .then(function () { dot.classList.add('on'); })
    .catch(function () { dot.classList.remove('on'); })
    .finally(function () { busy = false; });
}
setInterval(send, 16);

function bind(el) {
  var bit = parseInt(el.dataset.bit || 0, 10);
  var c   = parseInt(el.dataset.cmd || 0, 10);
  function on(e) {
    e.preventDefault();
    /* EXIT (65535) repeats while held, mirroring a held gamepad R1: the
       payload needs ~60 consecutive command frames to quit. MENU stays a
       single shot. */
    if (c === 65535) { hold_cmd = c; } else if (c) { cmd = c; } else { mask |= bit; }
    el.classList.add('hit'); dirty = true;
  }
  function off(e) {
    e.preventDefault();
    if (c === 65535) { hold_cmd = 0; } else if (!c) { mask &= ~bit; }
    el.classList.remove('hit'); dirty = true;
  }
  el.addEventListener('touchstart', on, { passive: false });
  el.addEventListener('touchend', off, { passive: false });
  el.addEventListener('touchcancel', off, { passive: false });
  el.addEventListener('mousedown', on);
  el.addEventListener('mouseup', off);
  el.addEventListener('mouseleave', off);
}
[].forEach.call(document.querySelectorAll('[data-bit],[data-cmd]'), bind);

// Keyboard. X = A, Z = B, Enter = START, Shift = SELECT, arrows = D-pad.
var KEYS = { KeyX: 1, KeyZ: 2, ShiftLeft: 4, ShiftRight: 4, Enter: 8,
             ArrowUp: 64, ArrowDown: 128, ArrowLeft: 32, ArrowRight: 16,
             KeyQ: 512, KeyW: 256 };
document.addEventListener('keydown', function (e) {
  if (KEYS[e.code]) { mask |= KEYS[e.code]; dirty = true; e.preventDefault(); }
});
document.addEventListener('keyup', function (e) {
  if (KEYS[e.code]) { mask &= ~KEYS[e.code]; dirty = true; e.preventDefault(); }
});

// Any attached gamepad, polled alongside the touch layer.
// Same arrangement as the native DualSense mapping in src/main.c, so a pad
// behaves identically whether it reaches the emulator through scePad or
// through this page: L1 = menu, R1 = quit (held), shoulders on L2/R2.
var GP = [
  [0, 1],     // south -> A
  [1, 2],     // east  -> B
  [8, 4],     // select
  [9, 8],     // start
  [6, 512],   // L2 -> L
  [7, 256],   // R2 -> R
  [12, 64], [13, 128], [14, 32], [15, 16]
];
setInterval(function () {
  var pads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (var i = 0; i < pads.length; i++) {
    var p = pads[i];
    if (!p) continue;
    var m = 0;
    for (var k = 0; k < GP.length; k++) {
      var b = p.buttons[GP[k][0]];
      if (b && b.pressed) m |= GP[k][1];
    }
    if (p.axes.length > 1) {
      if (p.axes[0] < -0.5) m |= 32;
      if (p.axes[0] >  0.5) m |= 16;
      if (p.axes[1] < -0.5) m |= 64;
      if (p.axes[1] >  0.5) m |= 128;
    }
    if (p.buttons[4] && p.buttons[4].pressed) cmd = 65534;
    if (p.buttons[5] && p.buttons[5].pressed) cmd = 65535;
    if (m !== mask) { mask = m; dirty = true; }
    return;
  }
}, 16);

// OFW ROM upload: the console has no save manager, so ROMs are POSTed to the
// payload and land in /temp0/roms/ (wiped on reboot -- re-upload after one).
// The picker rescans every time it opens, so hit MENU after the upload lands.
var romfile = document.getElementById('romfile'), ust = document.getElementById('ust');
romfile.addEventListener('change', function () {
  var f = romfile.files[0];
  if (!f) return;
  ust.textContent = 'sending ' + f.name + '...';
  fetch('/rom?name=' + encodeURIComponent(f.name), { method: 'POST', body: f })
    .then(function (r) {
      ust.textContent = r.ok ? 'ok: ' + f.name + ' (MENU to refresh)'
                             : 'rejected: HTTP ' + r.status;
    })
    .catch(function () { ust.textContent = 'send failed'; });
  romfile.value = '';
});
</script>
</body>
</html>
]=]

local html_resp = "HTTP/1.1 200 OK\r\nContent-Type:text/html\r\nConnection:close\r\n\r\n" .. html_body
local html_len  = #html_resp
local html_mem  = malloc(html_len + 16)
write_buffer(html_mem, html_resp)
ulog("HTML: " .. html_len .. " bytes")

-- The payload blob: code plus the initial contents of .data.
-- The blob arrives over TCP as raw binary rather than hex inside this script.
-- Luac0re's remote lua loader caps scripts at 500KB (remotelualoader.lua), and
-- hex encoding doubles the payload, so an embedded blob is limited to ~240KB.
-- A 663KB script was accepted by the socket and then silently never ran.
-- Receiving separately lifts the ceiling to 4MB and makes the script tiny.
--
-- This is write_shellcode_network() inlined: the library version memcpy's to a
-- single destination, but a multi-mapping payload needs the bytes left in
-- SHELLCODE_SCRATCH so each mapping can take its own slice.

local BLOB_PORT_LO, BLOB_PORT_HI = 9028, 9045
local BLOB_PORT = 9028

local function recv_blob()
    local sa2 = malloc(16)
    local en2 = malloc(4)
    local function htons2(p) return ((p << 8) | (p >> 8)) & 0xFFFF end

    local srv = create_socket(AF_INET, SOCK_STREAM, 0)
    ulog("blob: socket fd=" .. tostring(srv))
    if srv < 0 then error("blob: create_socket failed") end

    -- SO_REUSEADDR only. SO_REUSEPORT lets a leaked listener from an earlier
    -- payload share the port and steal the incoming connection, which hangs
    -- this accept() and wedges the loader.
    write32(en2, 1)
    syscall.setsockopt(srv, SOL_SOCKET, 0x0004, en2, 4)   -- SO_REUSEADDR

    -- Walk a range. A script that dies after bind() leaves its listener behind
    -- for the rest of the game session, so a fixed port means one crash costs a
    -- relaunch. The sender walks the same range and identifies the live one by
    -- its ACK.
    local bound = -1
    for p = BLOB_PORT_LO, BLOB_PORT_HI do
        write8(sa2 + 1, AF_INET)
        write16(sa2 + 2, htons2(p))
        write32(sa2 + 4, INADDR_ANY)
        if syscall.bind(srv, sa2, 16) == 0 then bound = p; break end
        ulog(string.format("blob: bind %d in use, trying next", p))
    end
    if bound < 0 then
        syscall.close(srv)
        error("blob: no free port -- relaunch to clear leaked listeners")
    end
    BLOB_PORT = bound
    ulog(string.format("blob: bind %d -> 0", BLOB_PORT))

    local lr = syscall.listen(srv, 1)
    ulog("blob: listen -> " .. tostring(lr))

    -- Timeout, never a bare accept(): a blocking accept that never fires wedges
    -- the Luac0re loader and costs a game relaunch.
    --
    -- fcntl + retry, NOT poll(): syscall.poll is absent from Luac0re's table,
    -- so calling it is a nil-call that kills the script with no log output.
    local F_SETFL, O_NONBLOCK = 4, 4
    syscall.fcntl(srv, F_SETFL, O_NONBLOCK)

    local ts = malloc(16)
    write64(ts, 0); write64(ts + 8, 50 * 1000 * 1000)   -- 50ms
    local function okfd(v) return v ~= nil and v >= 0 and v < 0x80000000 end

    ulog(string.format("blob: waiting on TCP %d (30s timeout)", BLOB_PORT))
    local cli = -1
    for _ = 1, 600 do
        cli = syscall.accept(srv, sa2, en2)
        if okfd(cli) then break end
        syscall.nanosleep(ts, 0)
    end
    if not okfd(cli) then
        syscall.close(srv)
        error("blob: no client within 30s")
    end
    -- Accepted socket must be blocking, or read() returns EAGAIN and the blob
    -- arrives truncated.
    syscall.fcntl(cli, F_SETFL, 0)

    -- A leaked listener accepts and buffers without complaint, then resets
    -- part-way through. Only a running script sends this byte back.
    local ackb = malloc(4)
    write8(ackb, 75)                    -- 'K'
    syscall.write(cli, ackb, 1)

    ulog("blob: accept fd=" .. tostring(cli))

    local maxsize, total = 0x400000, 0
    while total < maxsize do
        local n = syscall.read(cli, SHELLCODE_SCRATCH + total, maxsize - total)
        if n == 0 then break end
        if n < 0 then
            syscall.close(cli); syscall.close(srv); error("blob: read error")
        end
        total = total + n
    end
    syscall.close(cli)
    syscall.close(srv)
    if total == 0 then error("blob: nothing received") end
    ulog(string.format("blob: received %d bytes", total))
    return total
end

local blob_len = recv_blob()


-- Reservation covers exactly the blob (code + .rela.dyn + .data's init image),
-- page-aligned. The writable .data/.bss region is NOT part of the JIT mapping:
-- _start maps it separately as plain anonymous RW memory, because PS5 JIT
-- shared memory is a scarce per-process resource -- inside ps2emu the pool
-- fits about 768KB, and the old 1MB reservation died with ENOMEM on chunk 4.
local JIT_SIZE = 0xB4000

local bfd  = jit_malloc(8)
local rwfd = jit_malloc(8)
local rxfd = jit_malloc(8)
local rwa  = jit_malloc(8)
local rxa  = malloc(8)
local nm   = jit_malloc(8)

-- Every step is checked before the next one uses its result. An unchecked
-- failure here is fatal to the whole game process: a null rw would be handed
-- to jit_memcpy and take ps2emu (and Luac0re with it) down, which costs a
-- relaunch and tells us nothing. Reporting and stopping is always better.
local function jitfail(what, extra)
    ulog(string.format("JIT FAIL at %s%s", what, extra and (" -- " .. extra) or ""))
    send_notification("GbaC0re\nJIT setup failed at\n" .. what)
    return nil
end

jit_write_buffer(nm, "gb4b")
-- Multi-mapping loader.
--
-- A single JIT mapping caps at 256KB, but a payload can be larger: mappings
-- land at adjacent addresses and code executes across the seam (proven by
-- probe_contig.lua, which put a jmp in the last 16 bytes of one mapping
-- targeting the next and got its magic value back).
--
-- So the blob is split into 256KB pieces, one mapping each, and the pieces are
-- required to be adjacent -- a flat linked image cannot survive a gap. If the
-- kernel hands back a non-adjacent address the load is aborted rather than
-- jumping into whatever happens to be there.
--
-- The final mapping is allocated at exactly the remaining size (page-aligned),
-- not a full chunk: every byte of JIT pool matters, and the pool has a hard
-- per-process ceiling.
--
-- Note the pool DEPLETES within a game session: every payload sent leaves its
-- mapping behind, so a fresh launch yields 3 mappings and a used session may
-- only yield 2.

local CHUNK    = 0x40000

local nmaps = (JIT_SIZE + CHUNK - 1) // CHUNK

ulog(string.format("JIT: blob %d bytes, reserve 0x%X -> %d mapping(s) of 0x%X",
                   blob_len, JIT_SIZE, nmaps, CHUNK))


local maps = {}
for i = 1, nmaps do
    jit_write32(bfd, 0); jit_write32(rwfd, 0); jit_write32(rxfd, 0)
    jit_write64(rwa, 0); write64(rxa, 0)

    -- Final mapping takes only what is left; both are page multiples.
    local sz = JIT_SIZE - (i - 1) * CHUNK
    if sz > CHUNK then sz = CHUNK end

    local rr = jit_sceKernelJitCreateSharedMemory(nm, sz, 7, bfd)
    local base_fd = jit_read32(bfd)
    if base_fd == 0 or base_fd == 0xFFFFFFFF then
        return jitfail("CreateSharedMemory",
            string.format("mapping %d of %d, ret=0x%X -- JIT pool exhausted; relaunch the game",
                          i, nmaps, rr & 0xFFFFFFFF))
    end

    jit_sceKernelJitCreateAliasOfSharedMemory(base_fd, PROT_READ | PROT_WRITE, rwfd)
    jit_sceKernelJitCreateAliasOfSharedMemory(base_fd, PROT_READ | PROT_EXECUTE, rxfd)
    local rw_fd, rx_fd = jit_read32(rwfd), jit_read32(rxfd)
    if rw_fd == 0 or rx_fd == 0 or rw_fd == 0xFFFFFFFF or rx_fd == 0xFFFFFFFF then
        return jitfail("CreateAliasOfSharedMemory", string.format("mapping %d", i))
    end

    jit_sceKernelJitMapSharedMemory(rw_fd, PROT_READ | PROT_WRITE, rwa)
    local rw = jit_read64(rwa)
    if rw == 0 then return jitfail("JitMapSharedMemory(RW)", string.format("mapping %d", i)) end

    local mfd = jit_send_recv_fd(rx_fd, NEW_JIT_SOCK, NEW_MAIN_SOCK)
    if mfd < 0 then return jitfail("send_recv_fd", string.format("mapping %d", i)) end
    sceKernelJitMapSharedMemory(mfd, PROT_READ | PROT_EXECUTE, rxa)
    local rx = read64(rxa)
    if rx == 0 then return jitfail("JitMapSharedMemory(RX)", string.format("mapping %d", i)) end

    if i > 1 then
        local want = maps[i-1].rx + maps[i-1].sz
        if rx ~= want then
            return jitfail("non-adjacent mapping",
                string.format("mapping %d at 0x%X, expected 0x%X -- a flat image cannot span a gap",
                              i, rx, want))
        end
    end

    maps[i] = { rw = rw, rx = rx, sz = sz }
    ulog(string.format("JIT: mapping %d/%d rw=0x%X rx=0x%X sz=0x%X", i, nmaps, rw, rx, sz))
end

for i = 1, nmaps do
    local off = (i - 1) * CHUNK
    local len = blob_len - off
    if len > CHUNK then len = CHUNK end
    jit_memcpy(maps[i].rw, SHELLCODE_SCRATCH + off, len)
end

local rx = maps[1].rx
ulog(string.format("Code: %d bytes written across %d mapping(s), entry rx=0x%X",
                   blob_len, nmaps, rx))

-- ext_args, mirroring struct ext_args in src/core.h.
--   +0x00 s64 status        +0x08 s64 step        +0x10 u32 frame_count
--   +0x18 s32 log_fd        +0x1C s32 pad_fd      +0x20 u8  log_addr[16]
--   +0x30 dbg[0] web listen fd     +0x38 dbg[1] HTML buffer
--   +0x40 dbg[2] HTML length       +0x48 dbg[3] userId
-- (dbg[4..] unused; no FTP server -- removed like LuaGB, the ROM picker
--  covers ROM management.)
local ext = malloc(0x80)
memset(ext, 0, 0x80)
write64(ext + 0x00, 0xDEAD)
write32(ext + 0x18, log_sock)
write32(ext + 0x1C, -1)
for i = 0, 15 do write8(ext + 0x20 + i, read8(log_sa + i)) end
write64(ext + 0x30, web_sock >= 0 and web_sock or -1)
write64(ext + 0x38, html_mem)
write64(ext + 0x40, html_len)
write64(ext + 0x48, userId)

local current_ip = get_current_ip()
ulog("IP: " .. tostring(current_ip))
send_notification("GbaC0re\nhttp://" .. tostring(current_ip) .. ":" .. WEB_PORT)

func_wrap(rx)(EBOOT_BASE, SCE_KERNEL_DLSYM, ext)

local status = read64(ext + 0x00)
local step   = read64(ext + 0x08)
local frames = read32(ext + 0x10)
ulog(string.format("Done. status=%d step=%d frames=%d", status, step, frames))
send_notification(string.format("GbaC0re done\nstatus %d step %d\n%d frames", status, step, frames))
