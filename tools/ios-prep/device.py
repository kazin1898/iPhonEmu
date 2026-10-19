"""Virtual units: one directory per device under a devices root, made from a prepared firmware directory.

A device is its own PPN page store (its storage size chosen at creation), its own identity (serial, ECID,
MACs from a random seed) and its own kboot bundles, built for that store's geometry. The firmware
directory (`ios_prep.py prepare`) is shared: every device made from one IPSW serves its system volume
from the same rootfs.hfs (marker pages, read-only), so a device costs only the pages it writes.

    <root>/<NAME>/device.json        what it is: firmware, IPSW, storage, seed, state
                  nand/              the page store (geometry.json, ch*-ce*.pages, markers.json)
                  install/           the install boot: it_install, it_keybag, ramdisk-install.dmg, kboot.bin
                  kboot.bin          the normal boot's bundle (and kboot.json), identity.json
                  logs/              serial and QEMU logs of the install boot and of the last boot
                  qemu.lock          the running QEMU's PID while one is on this store

ios_prep.py's `device` commands drive this (docs/n94/README.md "Virtual devices").
"""
import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
WORKSPACE = os.path.dirname(REPO)
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
FORMAT = "iphonemu-device-v1"


def default_root():
    """IPHONEMU_DEVICES, else devices/ beside the repository (as firmware/ and runs/ are)."""
    return os.environ.get("IPHONEMU_DEVICES") or os.path.join(WORKSPACE, "devices")


def default_firmware(board, build):
    """firmware/<board without 'ap'>_<build> beside the repository: n94_10B329 for the 4S on 6.1.3."""
    return os.path.join(WORKSPACE, "firmware", f"{board.lower().removesuffix('ap')}_{build}")


def check_name(name):
    if not NAME_RE.match(name or ""):
        raise SystemExit(f"device name {name!r}: letters, digits, '.', '_' and '-', 64 at most, "
                         "not starting with a dot or a dash")
    return name


class Device:
    def __init__(self, root, name):
        self.root = os.path.abspath(root)
        self.name = check_name(name)
        self.dir = os.path.join(self.root, name)
        self.nand = os.path.join(self.dir, "nand")
        self.install = os.path.join(self.dir, "install")
        self.kboot = os.path.join(self.dir, "kboot.bin")
        self.logs = os.path.join(self.dir, "logs")
        self.meta_path = os.path.join(self.dir, "device.json")
        self.lock_path = os.path.join(self.dir, "qemu.lock")

    def exists(self):
        return os.path.isfile(self.meta_path)

    def load(self):
        try:
            with open(self.meta_path) as f:
                meta = json.load(f)
        except FileNotFoundError:
            raise SystemExit(f"no device {self.name!r} under {self.root}") from None
        if meta.get("format") != FORMAT:
            raise SystemExit(f"{self.meta_path}: not an iPhonEmu device")
        return meta

    def save(self, meta):
        meta["format"] = FORMAT
        tmp = self.meta_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(meta, f, indent=1)
        os.replace(tmp, self.meta_path)

    def running(self):
        """The PID of the QEMU on this store, or None (a lock left by a QEMU that is gone is stale)."""
        try:
            with open(self.lock_path) as f:
                pid = json.load(f)["pid"]
        except (FileNotFoundError, ValueError, KeyError):
            return None
        return pid if qemu_alive(pid) else None


def list_devices(root):
    root = os.path.abspath(root)
    if not os.path.isdir(root):
        return []
    out = []
    for name in sorted(os.listdir(root), key=str.lower):
        if NAME_RE.match(name) and os.path.isfile(os.path.join(root, name, "device.json")):
            out.append(Device(root, name))
    return out


def remove(dev):
    """Delete the device's directory, and nothing else: only a directory holding a device.json, directly
    under its root, with no QEMU on it."""
    if not dev.exists():
        raise SystemExit(f"no device {dev.name!r} under {dev.root}")
    if os.path.dirname(os.path.abspath(dev.dir)) != dev.root:
        raise SystemExit(f"{dev.dir}: not directly under {dev.root}")
    pid = dev.running()
    if pid:
        raise SystemExit(f"device {dev.name!r} is running (QEMU pid {pid}): quit it first")
    shutil.rmtree(dev.dir)


# ---- processes ---------------------------------------------------------------------------------------

def qemu_alive(pid):
    """Is pid a live qemu-system process? (Not os.kill(pid, 0): on Windows that terminates it.)"""
    if sys.platform != "win32":
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        try:
            with open(f"/proc/{pid}/comm") as f:
                return "qemu" in f.read()
        except OSError:
            return True
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = ctypes.c_void_p
    h = k32.OpenProcess(0x1000, False, pid)        # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    try:
        h = ctypes.c_void_p(h)
        code = ctypes.c_ulong()
        if not k32.GetExitCodeProcess(h, ctypes.byref(code)) or code.value != 259:     # STILL_ACTIVE
            return False
        buf = ctypes.create_unicode_buffer(1024)
        size = ctypes.c_ulong(len(buf))
        if not k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            return True
        return "qemu" in os.path.basename(buf.value).lower()
    finally:
        k32.CloseHandle(h)


def qemu_exe():
    exe = os.path.join(REPO, "build", "qemu-system-arm" + (".exe" if sys.platform == "win32" else ""))
    if not os.path.isfile(exe):
        raise SystemExit(f"{exe}: build QEMU first (RUNNING.md)")
    return exe


def qemu_env():
    """The environment for the Windows build: the MSYS2 UCRT64 DLLs it links against on the PATH."""
    env = dict(os.environ)
    ucrt = os.path.join(WORKSPACE, "toolchain", "msys64", "ucrt64", "bin")
    if sys.platform == "win32" and os.path.isdir(ucrt):
        env["PATH"] = ucrt + os.pathsep + env.get("PATH", "")
    return env


def machine(kboot, nand, usb=None):
    return ["-M", f"iPhone-4S,kboot={fwd(kboot)},nand={fwd(nand)}" + (f",usb-tcp-addr={usb}" if usb else "")]


USB_LINK = "127.0.0.1:1235"          # where QEMU's tcp_usb link dials the bridge
USBMUXD = ("127.0.0.1", 27015)       # where libimobiledevice looks for usbmuxd on Windows


def start_usb_bridge(dev, log=print):
    """tools/dev/usbmux_bridge.py for dev, unless one is already serving USBMUXD: the host's usbmuxd for
    the guest (ideviceinfo, idevicepair, AFC), pair records in <dev>/lockdown. Returns its Popen or None."""
    import socket
    try:
        socket.create_connection(USBMUXD, timeout=0.5).close()
        log(f"usbmux: a bridge already serves {USBMUXD[0]}:{USBMUXD[1]}; using it")
        return None
    except OSError:
        pass
    bridge = os.path.join(REPO, "tools", "dev", "usbmux_bridge.py")
    out = open(os.path.join(dev.logs, "usbmux-bridge.log"), "wb")
    kw = {}
    if sys.platform == "win32":
        kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS
    proc = subprocess.Popen([sys.executable, "-I", "-u", bridge, "--usb", USB_LINK,
                             "--mux", "%s:%d" % USBMUXD, "--records", os.path.join(dev.dir, "lockdown")],
                            stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT, **kw)
    out.close()
    log(f"usbmux: bridge pid {proc.pid} on {USBMUXD[0]}:{USBMUXD[1]} (libimobiledevice tools: "
        f"OPENSSL_CONF={os.path.join(REPO, 'tools', 'dev', 'openssl-ios6.cnf')})")
    return proc


def fwd(path):
    return os.path.abspath(path).replace(os.sep, "/")


class Lock:
    """qemu.lock with the QEMU's PID, for as long as it runs: one QEMU per page store."""

    def __init__(self, dev, what):
        self.dev, self.what = dev, what
        pid = dev.running()
        if pid:
            raise SystemExit(f"device {dev.name!r} already has a QEMU (pid {pid}); one QEMU per store")

    def hold(self, pid):
        with open(self.dev.lock_path, "w") as f:
            json.dump({"pid": pid, "what": self.what, "since": time.strftime("%Y-%m-%d %H:%M:%S")}, f)

    def release(self):
        try:
            os.remove(self.dev.lock_path)
        except FileNotFoundError:
            pass


DONE, FAIL = "it_install: done", "it_install: FAIL"
HALT_WAIT = 90
SHOW = ("it_install", "it_keybag", "FTL_Open", "Creating block device", "panic(")


def run_install_boot(dev, limit, log=print, poll=2.0):
    """The install boot on dev's store until the helper says done (or FAIL, or the kernel panics). After
    "done" the helper halts: the kernel flushes the FTL and the NVRAM and the PMU model powers the machine
    off, so QEMU exits by itself; it is quit through the monitor on stdin (a pipe: a fifo does not reach
    QEMU's monitor on Windows) only if it has not within HALT_WAIT seconds, or on FAIL or a panic (the FTL
    then opens the next boot with "unclean shutdown"). Returns (result line or None, seconds). The
    helper's own lines are passed to `log` as they come."""
    os.makedirs(dev.logs, exist_ok=True)
    serial = os.path.join(dev.logs, "install-serial.log")
    qlog = os.path.join(dev.logs, "install-qemu.log")
    for p in (serial, qlog):
        if os.path.exists(p):
            os.remove(p)
    cmd = [qemu_exe()] + machine(os.path.join(dev.install, "kboot.bin"), dev.nand) + [
        "-serial", "file:" + fwd(serial), "-display", "none", "-monitor", "stdio",
        "-d", "guest_errors", "-D", fwd(qlog)]
    lock = Lock(dev, "install")
    t0 = time.time()
    out_path = os.path.join(dev.logs, "install-qemu.out")
    with open(out_path, "wb") as out:      # a file, not a pipe nobody drains
        proc = subprocess.Popen(cmd, cwd=REPO, env=qemu_env(), stdin=subprocess.PIPE, stdout=out,
                                stderr=subprocess.STDOUT)
    lock.hold(proc.pid)
    result, seen, last_note = None, 0, t0
    try:
        while time.time() - t0 < limit and proc.poll() is None:
            time.sleep(poll)
            try:
                with open(serial, "rb") as f:
                    f.seek(seen)
                    chunk = f.read()
            except FileNotFoundError:
                continue
            cut = chunk.rfind(b"\n") + 1
            seen += cut
            for line in chunk[:cut].decode("utf-8", "replace").splitlines():
                if any(k in line for k in SHOW) and "it_install: at " not in line:
                    log(f"  [{time.time() - t0:5.0f}s] {line.strip()}")
                if DONE in line or FAIL in line or "panic(cpu" in line:
                    result = line.strip()
            if result:
                if DONE in result:
                    try:
                        proc.wait(HALT_WAIT)    # the helper's halt powers the machine off
                        log(f"  [{time.time() - t0:5.0f}s] halted (the FTL flushed, the machine off)")
                    except subprocess.TimeoutExpired:
                        log(f"  [{time.time() - t0:5.0f}s] no power-off after {HALT_WAIT} s: quitting QEMU")
                break
            if time.time() - last_note >= 120:
                last_note = time.time()
                log(f"  [{last_note - t0:5.0f}s] install boot running (a blank store formats first: about a "
                    "minute per GiB)")
        if proc.poll() is None:
            try:
                proc.stdin.write(b"quit\n")
                proc.stdin.flush()
                proc.wait(30)
            except (OSError, subprocess.TimeoutExpired):
                proc.kill()
                proc.wait()
        if result is None and proc.returncode not in (0, None):
            with open(out_path, "rb") as f:
                err = f.read().decode("utf-8", "replace").strip()
            if err:
                log(f"  qemu: {err.splitlines()[-1]}")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        lock.release()
    return result, time.time() - t0


def display_option(display):
    """QEMU's -display for a unit's window. The machine's touch panel is an absolute pointer, and QEMU's
    SDL and GTK windows hide the host's cursor over an absolute pointer that has no guest cursor of its own
    (iOS draws none), so nothing showed where a click would land: show-cursor=on keeps the host's cursor.
    An explicit show-cursor in `display`, and the window-less displays, are left as given."""
    kind = display.split(",", 1)[0]
    if kind in ("sdl", "gtk") and "show-cursor=" not in display:
        return display + ",show-cursor=on"
    return display


def boot(dev, display="sdl", qmp=4444, serial=None, extra=(), detach=False, usb=True):
    """The normal boot: dev's kboot.bin on dev's store, QMP on qmp and qmp + 1 (tools/dev/n94_input.py),
    and with usb the USB link to a usbmux bridge (start_usb_bridge), so the host's libimobiledevice
    reaches the unit. Waits for QEMU to exit (the window closed, or `n94_input.py quit`) unless detach;
    returns its exit status, or its PID when detached."""
    os.makedirs(dev.logs, exist_ok=True)
    serial = serial or "file:" + fwd(os.path.join(dev.logs, "serial.log"))
    if usb:
        start_usb_bridge(dev)
    cmd = [qemu_exe()] + machine(dev.kboot, dev.nand, USB_LINK if usb else None) + [
        "-serial", serial, "-display", display_option(display), "-monitor", "none"]
    if qmp:
        for port in (qmp, qmp + 1):
            cmd += ["-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off"]
    cmd += ["-d", "guest_errors", "-D", fwd(os.path.join(dev.logs, "qemu.log"))] + list(extra)
    lock = Lock(dev, "boot")
    out = open(os.path.join(dev.logs, "qemu.out"), "wb")
    kw = {}
    if detach and sys.platform == "win32":
        kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS
    proc = subprocess.Popen(cmd, cwd=REPO, env=qemu_env(), stdin=subprocess.DEVNULL, stdout=out,
                            stderr=subprocess.STDOUT, **kw)
    out.close()
    lock.hold(proc.pid)
    if detach:
        return proc.pid         # the lock goes stale when QEMU exits (running() checks the PID)
    try:
        return proc.wait()
    finally:
        lock.release()
