"""Edits to the system volume image (rootfs.hfs) the page store serves to the guest.

The IPSW's root is mounted read-only by the guest and its pages come from rootfs.hfs on the host
(hw/arm/s5l8940_iop.c marker pages), so a change here is what the guest reads at its next boot. What
is edited is launchd's job plists, the way qemu-ios does for the A4 boards (docs/research/userland-boot.md):

- `console`: the job's stdout and stderr to /dev/console, so a daemon's own messages ride the serial
  console (CoreAnimation's and backboardd's complaints are stderr; the console is `crw--w--w-`, so
  `mobile` can append). A path instead sends them to a file: on the data volume, its text can be read
  back from the NAND page store, which keeps the pages in the clear (XOR 0xff).
- `env`: an EnvironmentVariables entry, e.g. CA_ENABLE_OGL=0 for QuartzCore's software renderer.

The plists stay binary (launchd reads either). The IPSW's root has no free block and its small files are
HFS-compressed inline (no data blocks of their own), so the first edit empties a donor file the running
system never reads (the software-update ramdisk, 20 MB) and the rewritten plists take blocks from it.
"""
import copy
import plistlib

import hfs

JOBS = "/System/Library/LaunchDaemons/%s.plist"
DONOR = "/usr/standalone/update/ramdisk/H4SURamDisk.dmg"     # used only by an OTA update


def edit_jobs(rootfs, jobs, console=True, env=(), program=None, interval=0, disabled=None, donor=DONOR, unset=()):
    """Edit the launchd jobs `jobs` (labels) on `rootfs`; returns {label: new plist dict}. `program`
    replaces the job's ProgramArguments (a stock binary of the system volume, e.g. /bin/launchctl list,
    whose job table on the console tells which daemons run and how the last ones exited) and `interval`
    sets StartInterval, launchd re-running the job every that many seconds. `disabled` True sets the
    plist's Disabled key, False removes it (the stock plists have none): a disabled job is not loaded and its Mach services are not
    registered, so a client's lookup fails at once instead of waiting on a daemon that never answers.
    `unset` removes those environment variables (and EnvironmentVariables, when it is left empty)."""
    out, stock = {}, show_jobs(rootfs, jobs)
    for label in jobs:
        d = copy.deepcopy(stock[label])
        if console:
            dest = console if isinstance(console, str) else "/dev/console"
            d["StandardOutPath"] = dest
            d["StandardErrorPath"] = dest
        for kv in env:
            k, _, val = kv.partition("=")
            d.setdefault("EnvironmentVariables", {})[k] = val
        for k in unset:
            d.get("EnvironmentVariables", {}).pop(k, None)
            if d.get("EnvironmentVariables") == {}:
                del d["EnvironmentVariables"]
        if program:
            d.pop("Program", None)
            d["ProgramArguments"] = list(program)
        if interval:
            d["StartInterval"] = int(interval)
        if disabled:
            d["Disabled"] = True
        elif disabled is not None:
            d.pop("Disabled", None)     # back to the stock job, which has no such key
        out[label] = d
    # A job already as asked is left alone, and the image is opened for writing only when one is not:
    # its blocks keep their place, and a QEMU serving the image read-only does not stand in the way.
    todo = [label for label in jobs if out[label] != stock[label]]
    if todo:
        v = hfs.Volume(rootfs, writable=True)
        try:
            if v.free_blocks < 64 and donor:
                v.truncate(donor)
            for label in todo:
                v.set_file_data(JOBS % label, plistlib.dumps(out[label], fmt=plistlib.FMT_BINARY))
        finally:
            v.close()
    return out


ASL_CONF = "/private/etc/asl.conf"
ASL_CONSOLE = "? [<= Level debug] file /dev/console format=bsd\n"


def asl_console(rootfs, donor=DONOR):
    """syslogd copies every message to /dev/console (asl.conf's `file` action): the user-space daemons'
    log on the serial console, where only the kernel's was. For bring-up; the ASL store keeps working."""
    v = hfs.Volume(rootfs, writable=True)
    try:
        if v.free_blocks < 64 and donor:
            v.truncate(donor)
        conf = v.read_file(ASL_CONF).decode()
        if ASL_CONSOLE not in conf:
            conf += "\n# iPhonEmu: everything to the serial console\n" + ASL_CONSOLE
            v.set_file_data(ASL_CONF, conf.encode())
        return conf
    finally:
        v.close()


# What every unit made from a 10B329 system volume needs (docs/n94/README.md "M3"): the Bluetooth jobs off
# (BlueTool's HCI traffic on uart3 ends in AppleSimpleUARTSync's assertion, a kernel panic). (jobs,
# edit_jobs keywords.) How CoreAnimation renders (the GL bridge, or CA_ENABLE_OGL=0 for QuartzCore's
# software renderer) is ios_prep.coreanimation's, per firmware.
UNIT_EDITS = (
    (("com.apple.BTServer", "com.apple.BlueTool"), {"disabled": True}),
)


def unit_edits(rootfs, donor=DONOR):
    """UNIT_EDITS on `rootfs`, idempotent (edit_jobs leaves a job already as asked alone); returns the
    labels of the jobs this call changed. A job the build does not have is skipped."""
    changed = []
    for jobs, kw in UNIT_EDITS:
        for label in jobs:
            try:
                before = show_jobs(rootfs, [label])[label]
            except FileNotFoundError:
                continue
            if edit_jobs(rootfs, [label], console=False, donor=donor, **kw)[label] != before:
                changed.append(label)
    return changed


LOCKDOWND = "/usr/libexec/lockdownd"


def activation_shortcut(rootfs, donor=DONOR):
    """lockdownd takes its development activation short-circuit unconditionally (lockdownd_patch.py): the
    unit reports Activated without an Apple-issued record, which a synthetic identity cannot have. The
    binary is stored uncompressed in place of its decmpfs copy; a second run finds it patched and leaves
    it. Returns the patcher's report."""
    import lockdownd_patch
    v = hfs.Volume(rootfs)
    try:
        old = v.read_file(LOCKDOWND)
    finally:
        v.close()
    new, report = lockdownd_patch.patch(old)
    if new == old:
        return report
    v = hfs.Volume(rootfs, writable=True)
    try:
        need = -(-len(new) // v.block_size)
        if v.free_blocks < need + 64 and donor:
            v.truncate(donor)
        v.set_file_data(LOCKDOWND, new)
        if v.read_file(LOCKDOWND) != new:
            raise OSError(f"{rootfs}: {LOCKDOWND} reads back different from what was written")
        return report
    finally:
        v.close()


SHARED_CACHE = "/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7"
AUDIO_SESSION_TIMEOUT_STOCK = 5000           # ms, 10B329
AUDIO_SESSION_TIMEOUT_EMULATOR = 60000


def _cache_maps(read):
    import struct
    u32 = lambda o: struct.unpack("<I", read(o, 4))[0]
    u64 = lambda o: struct.unpack("<Q", read(o, 8))[0]
    mapoff, mapcnt, imgoff, imgcnt = u32(16), u32(20), u32(24), u32(28)
    maps = [(u64(mapoff + 32 * i), u64(mapoff + 32 * i + 8), u64(mapoff + 32 * i + 16)) for i in range(mapcnt)]
    images = [(u64(imgoff + 32 * i), u32(imgoff + 32 * i + 24)) for i in range(imgcnt)]
    return maps, images


def _thumb_imm16(hw1, hw2):
    """The 16-bit immediate of a Thumb-2 MOVW/MOVT."""
    return ((hw1 & 0xf) << 12) | (((hw1 >> 10) & 1) << 11) | (((hw2 >> 12) & 7) << 8) | (hw2 & 0xff)


def _pc_relative_targets(text, base):
    """Addresses a Thumb-2 image builds with movw rd; movt rd; ...; add rd, pc (the cache's way to reach
    its data), as {address: count}."""
    import struct
    out = {}
    n = len(text) // 2
    hw = struct.unpack("<%dH" % n, text[:2 * n])
    for i in range(n - 6):
        a = hw[i]
        if a & 0xfbf0 != 0xf240:
            continue
        rd = (hw[i + 1] >> 8) & 0xf
        if rd > 7 or hw[i + 2] & 0xfbf0 != 0xf2c0 or (hw[i + 3] >> 8) & 0xf != rd:
            continue
        imm = _thumb_imm16(a, hw[i + 1]) | _thumb_imm16(hw[i + 2], hw[i + 3]) << 16
        for j in range(i + 4, min(i + 12, n)):
            if hw[j] == 0x4478 | rd:                     # add rd, pc
                t = (imm + base + 2 * j + 4) & 0xffffffff
                out[t] = out.get(t, 0) + 1
                break
    return out


def audio_session_timeout(rootfs, ms=AUDIO_SESSION_TIMEOUT_EMULATOR):
    """The timeout of AudioToolbox's audio-session calls into mediaserverd (AudioSessionSetProperty and its
    kin: MIG requests sent with MACH_RCV_TIMEOUT), 5000 ms in 10B329, raised to `ms` in the shared cache's
    __DATA, where every process takes its initial value from.

    When the reply does not come in time (MACH_RCV_TIMED_OUT), AudioToolbox writes a stackshot report and
    the caller kills itself (FigRPCTimeout_KillAndForceCrashReport: "mediaserverd: RPCTimeout message
    received to terminate [0] with reason 'AudioSessionSetProperty'"). The emulator runs the A5's two
    cores as one TCG vCPU, and at boot mediaserverd spends about 27 s in its audio initialization
    (CoreAudio's HAL plugins, CommCenter asked for the vocoder) before it serves sessions: assistantd's
    first AudioSessionSetProperty waited past 5 s and assistantd was killed on every boot. At 60 s the
    watchdog keeps its job at the emulator's pace. (Caught with a gdbstub breakpoint on the kill: the
    mach_msg returned 0x10004003 with 5000 on the stack. CoreMedia's gFigRPCTimeout, AudioToolbox's other
    9 s limit and the com.apple.coremedia "rpc_timeout" preference were tried first: other calls.)

    The global is found, with no address assumed, as the one word holding the stock value that
    AudioToolbox's code reaches through a non-lazy pointer it builds pc-relatively and that lies in its
    own image's data. Idempotent; returns the report."""
    import struct
    v = hfs.Volume(rootfs)
    try:
        rec = v.resolve(v.lookup(SHARED_CACHE))
        if rec.compressed:
            raise ValueError(f"{SHARED_CACHE} is compressed")

        def read(off, n):
            v.f.seek(v.fork_offset(rec.data, off))
            return v.f.read(n)
        maps, images = _cache_maps(read)

        def a2o(addr):
            for a, size, off in maps:
                if a <= addr < a + size:
                    return addr - a + off
            return None
        u32 = lambda o: struct.unpack("<I", read(o, 4))[0]
        at = [(a, p) for a, p in images if read(p, 128).split(b"\0")[0].endswith(b"/AudioToolbox")]
        if len(at) != 1:
            raise LookupError("AudioToolbox not in the shared cache")
        mh = a2o(at[0][0])
        segs, p = {}, mh + 28
        for _ in range(u32(mh + 16)):
            cmd, size = u32(p), u32(p + 4)
            if cmd == 1:                                    # LC_SEGMENT
                segs[read(p + 8, 16).split(b"\0")[0]] = (u32(p + 24), u32(p + 28))
            p += size
        text, data = segs[b"__TEXT"], segs[b"__DATA"]
        found = set()
        for slot in _pc_relative_targets(read(a2o(text[0]), text[1]), text[0]):
            so = a2o(slot)
            if so is None:
                continue
            ptr = u32(so)
            if data[0] <= ptr < data[0] + data[1] and a2o(ptr) is not None and u32(a2o(ptr)) in (
                    AUDIO_SESSION_TIMEOUT_STOCK, ms):
                found.add(ptr)
        if len(found) != 1:
            raise LookupError(f"AudioToolbox's session timeout: {len(found)} candidates, not one")
        glob = found.pop()
        off = a2o(glob)
        pos = v.fork_offset(rec.data, off)
        old = u32(off)
    finally:
        v.close()
    if old == ms:
        return f"AudioToolbox's session timeout already {ms} ms"
    with open(rootfs, "r+b") as f:
        f.seek(pos)
        f.write(struct.pack("<I", ms))
    return f"AudioToolbox's session timeout (global {glob:#x}) {old} -> {ms} ms"


def show_jobs(rootfs, jobs):
    v = hfs.Volume(rootfs)
    try:
        return {label: plistlib.loads(v.read_file(JOBS % label)) for label in jobs}
    finally:
        v.close()
