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


def edit_jobs(rootfs, jobs, console=True, env=(), program=None, interval=0, disabled=None, donor=DONOR):
    """Edit the launchd jobs `jobs` (labels) on `rootfs`; returns {label: new plist dict}. `program`
    replaces the job's ProgramArguments (a stock binary of the system volume, e.g. /bin/launchctl list,
    whose job table on the console tells which daemons run and how the last ones exited) and `interval`
    sets StartInterval, launchd re-running the job every that many seconds. `disabled` True sets the
    plist's Disabled key, False removes it (the stock plists have none): a disabled job is not loaded and its Mach services are not
    registered, so a client's lookup fails at once instead of waiting on a daemon that never answers."""
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


# What every unit made from a 10B329 system volume needs (docs/n94/README.md "M3"): QuartzCore's software
# renderer for backboardd and SpringBoard (no SGX model), and the Bluetooth jobs off (BlueTool's HCI
# traffic on uart3 ends in AppleSimpleUARTSync's assertion, a kernel panic). (jobs, edit_jobs keywords).
UNIT_EDITS = (
    (("com.apple.backboardd", "com.apple.SpringBoard"), {"env": ("CA_ENABLE_OGL=0",)}),
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


def show_jobs(rootfs, jobs):
    v = hfs.Volume(rootfs)
    try:
        return {label: plistlib.loads(v.read_file(JOBS % label)) for label in jobs}
    finally:
        v.close()
