#!/usr/bin/env python3
"""ios-prep: turn an Apple IPSW into the kernel bundle the iPhonEmu machines boot.

  ios_prep.py info      IPSW                      what the IPSW is (device, build, components)
  ios_prep.py prepare   IPSW --out DIR [options]   decrypt kernel/DT/iBoot, expand the rootfs, build kboot.bin
  ios_prep.py kboot     DIR [options]              rebuild kboot.bin from a prepared directory
                        (--ramdisk restore|update --ipsw IPSW: boot the IPSW's ramdisk as root, rd=md0;
                         --ramdisk-file IMAGE: that image instead; --bundle FILE: the output's place)
  ios_prep.py nand      --out DIR                  a blank PPN page store for -M iPhone-4S,nand=DIR
  ios_prep.py install   DIR --nand NAND --out OUT  the install boot: it_install and it_keybag built for this
                        rootfs, markers.json in the store, the restore ramdisk with the helpers (docs/n94/README.md)
  ios_prep.py inventory DIR [--docs DOCS]          device-tree dump and the kernelcache's kext list
  ios_prep.py device create NAME --ipsw IPSW [--storage 16G]   a virtual unit, IPSW to installed in one go
  ios_prep.py device boot|refresh|delete NAME / device list    use it, re-run its install boot, remove it
                        (--root DIR or IPHONEMU_DEVICES: where the units live; docs/n94/README.md)

Needs the build's keys: --keys JSON, else the cache keys_<build>_<device>.json beside the IPSW, else
The Apple Wiki (keys.py). Runs on Windows, macOS and Linux with Python 3 and `cryptography`.
"""
import argparse
import hashlib
import io
import json
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import img3  # noqa: E402
import kboot  # noqa: E402
import nand_ppn  # noqa: E402
import guest_install  # noqa: E402
import keys as keymod  # noqa: E402
import lzss  # noqa: E402
from devicetree import DeviceTree  # noqa: E402
from ipsw import IPSW  # noqa: E402
from macho import MachO  # noqa: E402


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def hfs_info(path):
    """The HFS+/HFSX volume header at 0x400, or an error string."""
    with open(path, "rb") as f:
        f.seek(0x400)
        h = f.read(512)
    sig = h[0:2]
    if sig not in (b"H+", b"HX"):
        return {"ok": False, "why": f"no HFS+ signature at 0x400 (got {sig!r})"}
    version, attrs, last_mounted = struct.unpack_from(">HII", h, 2)
    file_count, folder_count, block_size, total_blocks, free_blocks = struct.unpack_from(">IIIII", h, 0x20)
    return {"ok": True, "signature": sig.decode(), "version": version, "block_size": block_size,
            "total_blocks": total_blocks, "free_blocks": free_blocks, "files": file_count, "folders": folder_count,
            "bytes": block_size * total_blocks, "last_mounted_by": last_mounted.to_bytes(4, "big").decode("latin-1")}


def decrypt_component(ip, keys, comp, out_path, expect=None):
    k = keys.get(comp)
    if not k:
        raise SystemExit(f"no key for {comp}")
    member = ip.find(k["file"])
    if not member:
        raise SystemExit(f"{k['file']} ({comp}) is not in the IPSW")
    raw = ip.read(member)
    plain = img3.decrypt(raw, bytes.fromhex(k["iv"]), bytes.fromhex(k["key"])) if raw[:4] == img3.MAGIC else raw
    if expect == "complzss":
        plain = lzss.complzss(plain)
    with open(out_path, "wb") as f:
        f.write(plain)
    log(f"{comp}: {k['file']} -> {os.path.basename(out_path)} ({len(plain)} bytes)")
    return plain


def expand_rootfs(ip, keys, out_dir, keep_dmg=False):
    k = keys.get("rootfs")
    if not k:
        raise SystemExit("no rootfs key")
    member = ip.find(k["file"])
    enc = os.path.join(out_dir, k["file"])
    dec = os.path.join(out_dir, "rootfs.dmg")
    hfs = os.path.join(out_dir, "rootfs.hfs")
    py = [sys.executable, "-I"]
    log(f"rootfs: extracting {k['file']} ({ip.size(member)} bytes)")
    ip.extract_to(member, enc)
    log("rootfs: VFDecrypt")
    subprocess.check_call(py + [os.path.join(HERE, "vfdecrypt.py"), enc, k["key"], dec])
    os.remove(enc)
    log("rootfs: UDIF -> raw HFS+")
    subprocess.check_call(py + [os.path.join(HERE, "udif.py"), "extract", dec, hfs, "Apple_HFS"])
    if not keep_dmg:
        os.remove(dec)
    info = hfs_info(hfs)
    if not info["ok"]:
        raise SystemExit(f"rootfs: {info['why']}")
    log(f"rootfs: {info['signature']} {info['bytes']} bytes, {info['files']} files, {info['folders']} folders")
    return hfs, info


def load_identity(out_dir, seed, board):
    path = os.path.join(out_dir, "identity.json")
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    ident = kboot.synthesize_identity(seed, board)
    with open(path, "w") as f:
        json.dump(ident, f, indent=2)
    return ident


def read(path):
    with open(path, "rb") as f:
        return f.read()


def ramdisk_image(out_dir, args, ip=None):
    """The decrypted restore/update ramdisk (raw HFS+), from DIR/ramdisk-<which>.dmg or the IPSW."""
    which = args.ramdisk
    if not which:
        return None
    path = getattr(args, "ramdisk_file", None) or os.path.join(out_dir, f"ramdisk-{which}.dmg")
    if not os.path.exists(path):
        if ip is None:
            if not args.ipsw:
                raise SystemExit(f"{path} is not there yet: pass --ipsw to decrypt it")
            ip = IPSW(args.ipsw)
        keys = keymod.load(ip.build, ip.product, os.path.dirname(os.path.abspath(ip.path)), getattr(args, "keys", None))
        comp = {"restore": "RestoreRamDisk", "update": "UpdateRamDisk"}[which]
        decrypt_component(ip, keys, comp, path)
    data = read(path)
    if data[0x400:0x402] not in (b"H+", b"HX"):
        raise SystemExit(f"{path} is not a raw HFS+ image (no H+/HX header at 0x400)")
    log(f"ramdisk: {os.path.basename(path)}, {len(data)} bytes")
    return data


def build_kboot(out_dir, args, ip=None):
    kernel = read(os.path.join(out_dir, "kernelcache.mach"))
    dtb = read(os.path.join(out_dir, "DeviceTree.bin"))
    iboot_path = os.path.join(out_dir, "iBoot.bin")
    iboot = read(iboot_path) if os.path.exists(iboot_path) else None
    board = kboot.board_of(DeviceTree(dtb))
    ident = load_identity(getattr(args, "identity_dir", None) or out_dir, args.seed, board)
    boot_args = args.boot_args or kboot.DEFAULT_BOOT_ARGS
    if args.verbose_boot and "-v" not in boot_args.split():
        boot_args += " -v"
    for extra in args.add_boot_arg or ():
        if extra not in boot_args.split():
            boot_args += " " + extra
    image, load_pa, entry, ba_pa, info = kboot.build(kernel, dtb, ident, boot_args=boot_args, iboot=iboot,
                                                      cpus=args.cpus, nand=args.nand, hide=args.hide or (),
                                                      ramdisk=ramdisk_image(out_dir, args, ip))
    segments = []
    logo_path = os.path.join(out_dir, "AppleLogo.bin")
    if os.path.exists(logo_path) and not getattr(args, "no_boot_logo", False):
        # the panel shows the logo from reset, as iBoot leaves it, until backboardd's first frame
        try:
            segments.append(kboot.logo_segment(board, read(logo_path)))
            info["boot_logo"] = {"pa": segments[0][0], "bytes": segments[0][1]}
        except ValueError as e:
            log(f"boot logo: {e}; the panel stays black until backboardd draws")
    blob = kboot.bundle(image, load_pa, entry, ba_pa, segments)
    out = getattr(args, "bundle", None) or os.path.join(out_dir, "kboot.bin")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(blob)
    info["kboot"] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
    info["boot_args"] = boot_args
    with open(os.path.splitext(out)[0] + ".json", "w") as f:
        json.dump(info, f, indent=2, default=lambda v: "0x%x" % v if isinstance(v, int) else v)
    log(f"kboot.bin: {len(blob)} bytes, load 0x{load_pa:08x} entry 0x{entry:08x} boot_args 0x{ba_pa:08x} "
        f"(Version {info['boot_args_version']}, {args.cpus} cpu(s), "
        + (f"nand=ppn {nand_ppn.raw_bytes(args.nand) >> 30} GiB" if isinstance(args.nand, dict) else f"nand={args.nand}")
        + (f", ramdisk {args.ramdisk} at 0x{info['ramdisk'][0]:08x})" if info.get("ramdisk") else ")"))
    return out, info


def prop_bytes(dt, props, key):
    if key not in props:
        return b""
    off, length = props[key]
    return bytes(dt.data[off + 36:off + 36 + length])


def write_inventory(out_dir, docs=None):
    dtb = read(os.path.join(out_dir, "DeviceTree.bin"))
    dt = DeviceTree(dtb)
    buf = io.StringIO()
    dt.dump(buf)
    with open(os.path.join(out_dir, "devicetree.txt"), "w") as f:
        f.write(buf.getvalue())
    m = MachO(read(os.path.join(out_dir, "kernelcache.mach")))
    kexts = m.prelink_bundle_ids()
    with open(os.path.join(out_dir, "kexts.txt"), "w") as f:
        f.write("\n".join(kexts) + "\n")
    # the arm-io address map: every block (shadowed duplicates included), compatible, absolute reg
    # windows and AIC interrupt numbers
    ranges = dt.u32s("arm-io", "ranges")
    base = ranges[1] - ranges[0] if len(ranges) >= 3 else 0
    rows = []
    for path, props in dt.records:
        if not path.startswith("arm-io/") or path.count("/") != 1:
            continue
        regb = prop_bytes(dt, props, "reg")
        reg = struct.unpack_from("<%dI" % (len(regb) // 4), regb)
        wins = ["0x%08x+0x%x" % (base + reg[i], reg[i + 1]) for i in range(0, len(reg) - 1, 2)]
        irqb = prop_bytes(dt, props, "interrupts")
        irq = [str(w) for w in struct.unpack_from("<%dI" % (len(irqb) // 4), irqb)] if len(irqb) % 4 == 0 else []
        compat = " | ".join(p.decode("ascii", "replace")
                            for p in prop_bytes(dt, props, "compatible").rstrip(b"\x00").split(b"\x00"))
        rows.append((path[7:], compat, ", ".join(wins), ", ".join(irq)))
    with open(os.path.join(out_dir, "iomap.tsv"), "w") as f:
        f.write("node\tcompatible\treg (absolute)\tAIC irqs\n")
        for r in rows:
            f.write("\t".join(r) + "\n")
    log(f"inventory: {len(dt.records)} DT nodes, {len(kexts)} kexts, {len(rows)} arm-io blocks")
    if docs:
        os.makedirs(docs, exist_ok=True)
        for name in ("devicetree.txt", "kexts.txt", "iomap.tsv"):
            with open(os.path.join(out_dir, name)) as s, open(os.path.join(docs, name), "w") as d:
                d.write(s.read())
    return rows, kexts, m.version_string()


def cmd_info(args):
    ip = IPSW(args.ipsw)
    print(f"{ip.product} {ip.version} ({ip.build}) board {ip.board} "
          f"CPID 0x{ip.device_map.get('CPID', 0):x} BDID {ip.device_map.get('BDID')}")
    for comp, path in ip.components().items():
        print(f"  {comp:28s} {path}  ({ip.size(path)} bytes)")
    n = sum(1 for p in ip.names if p.endswith(".img3") or p.endswith(".dfu"))
    print(f"  {n} IMG3/DFU images under Firmware/")


def cmd_prepare(args):
    ip = IPSW(args.ipsw)
    out = args.out
    os.makedirs(out, exist_ok=True)
    log(f"{ip.product} {ip.version} ({ip.build}), board {ip.board} -> {out}")
    keys = keymod.load(ip.build, ip.product, os.path.dirname(os.path.abspath(args.ipsw)), args.keys)
    log(f"keys: {len([k for k in keys if not k.startswith('_')])} components" +
        (f" from {keys['_source']}" if "_source" in keys else ""))
    hashes = {"ipsw": {"file": os.path.basename(args.ipsw), "sha256": sha256_file(args.ipsw)}}
    kc = decrypt_component(ip, keys, "kernelcache", os.path.join(out, "kernelcache.mach"), expect="complzss")
    m = MachO(kc)
    log(f"kernel: {m.version_string()}; entry 0x{m.entry():08x}; boot_args Version {m.boot_args_version()}")
    dtb = decrypt_component(ip, keys, "DeviceTree", os.path.join(out, "DeviceTree.bin"))
    dt = DeviceTree(dtb)
    log(f"device tree: {' / '.join(dt.strings('', 'compatible'))}, {len(dt.records)} nodes")
    for comp, name in (("iBoot", "iBoot.bin"), ("AppleLogo", "AppleLogo.bin"), ("LLB", "LLB.bin")):
        if comp in keys:
            decrypt_component(ip, keys, comp, os.path.join(out, name))
    if not args.skip_rootfs:
        if os.path.exists(os.path.join(out, "rootfs.hfs")) and not args.redo_rootfs:
            log("rootfs: rootfs.hfs already there (--redo-rootfs to rebuild)")
            hashes["rootfs"] = hfs_info(os.path.join(out, "rootfs.hfs"))
        else:
            hfs, info = expand_rootfs(ip, keys, out, keep_dmg=args.keep_dmg)
            hashes["rootfs"] = info
        hashes.update(firmware_edits(os.path.join(out, "rootfs.hfs"), args))
    write_inventory(out, args.docs)
    build_kboot(out, args, ip)
    for name in ("kernelcache.mach", "DeviceTree.bin", "iBoot.bin", "kboot.bin", "rootfs.hfs"):
        p = os.path.join(out, name)
        if os.path.exists(p):
            hashes[name] = {"bytes": os.path.getsize(p), "sha256": sha256_file(p)}
    with open(os.path.join(out, "hashes.json"), "w") as f:
        json.dump(hashes, f, indent=2)
    log("done")


def firmware_edits(rootfs, args):
    """What every unit made from this system volume needs, on rootfs.hfs (per firmware, so once for all
    of them), each idempotent: lockdownd's activation short-circuit (unless --stock-lockdownd), the
    launchd jobs of sysvol.UNIT_EDITS, the M3 edits, and AudioToolbox's audio-session timeout raised to
    the emulator's pace (sysvol.audio_session_timeout; both unless --stock-jobs). Returns hashes.json's
    record."""
    import sysvol
    rec = {}
    if not getattr(args, "stock_lockdownd", False):
        rec["activation"] = activation_shortcut(rootfs)
    if not getattr(args, "stock_jobs", False):
        changed = sysvol.unit_edits(rootfs)
        rec["unit_edits"] = [f"{', '.join(jobs)}: {kw}" for jobs, kw in sysvol.UNIT_EDITS]
        log("launchd jobs: " + (f"{', '.join(changed)} edited" if changed else "already as every unit needs them")
            + " (CA_ENABLE_OGL=0 for backboardd and SpringBoard, BTServer and BlueTool disabled)")
        rec["audio_session_timeout"] = sysvol.audio_session_timeout(rootfs)
        log(f"shared cache: {rec['audio_session_timeout']}")
    return rec


def activation_shortcut(rootfs):
    """lockdownd's activation short-circuit on the system volume (sysvol.activation_shortcut): what every
    virtual unit made from this firmware needs to get past Setup's activation page, since none has an
    Apple-issued record. Idempotent. On a lockdownd the patcher does not recognize (another build) it logs
    why and leaves the binary stock instead of failing the preparation; returns the report."""
    import lockdownd_patch
    import sysvol
    try:
        report = sysvol.activation_shortcut(rootfs)
    except lockdownd_patch.PatchError as e:
        report = f"stock lockdownd kept ({e}); Setup may stop at its activation page"
    log(f"lockdownd: {report}")
    return report


def cmd_kboot(args):
    if args.nand_store:
        args.nand = store_geometry(args.nand_store)
    build_kboot(args.dir, args)


def store_geometry(store):
    """A page store's geometry.json: what the kernel's device tree must say for that store."""
    with open(os.path.join(store, "geometry.json")) as f:
        return json.load(f)


def cmd_nand(args):
    g = nand_ppn.geometry(storage_gib(args.storage))
    if args.ptab_only:
        with open(os.path.join(args.out, "geometry.json")) as f:
            g = json.load(f)
    path = nand_ppn.write_store(args.out, g, ptab_only=args.ptab_only)
    log(f"PPN page store: {path}, {nand_ppn.raw_bytes(g) >> 30} GiB ({g['channels']}x{g['ce_per_channel']} CEs, "
        f"{g['caus_per_ce']} CAUs x {g['blocks_per_cau']} blocks x "
        f"{g['pages_per_block']} x {g['page_bytes']} B), the partition table in the boot "
        f"blocks: " + ", ".join(f"{n}:{a}:{b}" for n, t, a, b, fl in nand_ppn.partition_specs(g)))
    if g != nand_ppn.PPN:
        log("a store of this size boots with a kboot.bin built for it: kboot ... --nand ppn --nand-store " + args.out)


def storage_gib(text):
    try:
        return nand_ppn.parse_storage(text)
    except ValueError as e:
        raise SystemExit(str(e)) from None


def cmd_install(args):
    """The install boot's pieces: the helper for this rootfs, markers.json in the store, the ramdisk."""
    rootfs = os.path.join(args.dir, "rootfs.hfs")
    stock = os.path.join(args.dir, "ramdisk-restore.dmg")
    if not os.path.exists(stock):
        ramdisk_image(args.dir, argparse.Namespace(ramdisk="restore", ramdisk_file=None, ipsw=args.ipsw,
                                                   keys=args.keys))
    os.makedirs(args.out, exist_ok=True)
    markers, blocks = guest_install.write_markers(args.nand, rootfs)
    log(f"markers: {markers} (source 0 = rootfs.hfs, {blocks} blocks of 4 KiB)")
    skeleton = guest_install.var_skeleton(rootfs)
    if args.activation == "factory":
        skeleton += guest_install.lockdown_seed(skeleton)
        log("lockdownd's data ark: FactoryActivated, BrickState false (/private/var/root/Library/Lockdown)")
    helper, info = guest_install.build_helper(blocks, args.out, clang=args.clang, lld=args.lld, skeleton=skeleton)
    log(f"it_install: {helper}, {info['code_bytes']} bytes of code in a {info['macho_bytes']}-byte Mach-O, "
        f"table lba {info['system_lba']} + {blocks} blocks, /private/var skeleton {len(skeleton)} entries")
    keybag, kinfo = guest_install.build_keybag_helper(args.out, clang=args.clang, lld=args.lld)
    log(f"it_keybag: {keybag}, {kinfo['code_bytes']} bytes of code in a {kinfo['macho_bytes']}-byte dyld-linked "
        f"Mach-O, binds {', '.join(kinfo['binds'])} from {kinfo['dylib']}")
    rd = guest_install.make_install_ramdisk(stock, os.path.join(args.out, "ramdisk-install.dmg"),
                                            [("/usr/local/bin/restored_external", helper), (kinfo["slot"], keybag)])
    log(f"ramdisk: {rd['ramdisk']}, {rd['bytes']} bytes, helpers at blocks {rd['helpers']}")
    with open(os.path.join(args.out, "install.json"), "w") as f:
        json.dump({"helper": info, "keybag_helper": kinfo, "ramdisk": rd, "markers": markers,
                   "system_blocks": blocks}, f, indent=2)
    log("next: kboot DIR --ramdisk restore --ramdisk-file OUT/ramdisk-install.dmg --bundle OUT/kboot.bin --nand ppn "
        "--add-boot-arg nand-enable-reformat=1, then boot with nand=NAND; stop at \"it_install: done\"")


def cmd_sysvol(args):
    """launchd jobs on the system volume image: console output, environment (sysvol.py)."""
    import sysvol
    rootfs = os.path.join(args.dir, "rootfs.hfs")
    if args.asl_console:
        sysvol.asl_console(rootfs)
        log(f"{rootfs}: asl.conf copies every message to /dev/console")
    if args.activation_shortcut and activation_shortcut(rootfs).startswith("stock"):
        raise SystemExit(1)
    if args.output:
        args.console = args.output
    if args.show or not (args.console or args.env or args.program or args.interval or args.disable is not None):
        jobs = sysvol.show_jobs(rootfs, args.job)
    else:
        jobs = sysvol.edit_jobs(rootfs, args.job, console=args.console, env=args.env,
                                program=args.program, interval=args.interval, disabled=args.disable)
        log(f"{rootfs}: {len(jobs)} job(s) rewritten; the guest reads them at its next boot")
    for label, d in jobs.items():
        print(label, {k: d[k] for k in ("Disabled", "ProgramArguments", "StartInterval", "StandardOutPath",
                                        "StandardErrorPath", "EnvironmentVariables") if k in d})


def cmd_inventory(args):
    rows, kexts, ver = write_inventory(args.dir, args.docs)
    print(ver)
    for r in rows:
        print("%-22s %-40s %s  irq %s" % r)


# ---- virtual devices (device.py) ----------------------------------------------------------------------

DEFAULT_SEED = "iPhonEmu n94 dev1"


def firmware_ready(fw):
    return all(os.path.exists(os.path.join(fw, n))
               for n in ("hashes.json", "kernelcache.mach", "DeviceTree.bin", "rootfs.hfs"))


def ensure_firmware(args, ip):
    """The prepared firmware directory for this IPSW, shared by every device made from it: prepared now if
    it is not there, else checked against the IPSW and given the per-firmware edits it may lack."""
    import device
    fw = os.path.abspath(args.firmware or device.default_firmware(ip.board, ip.build))
    if not firmware_ready(fw):
        log(f"firmware: preparing {fw} from {os.path.basename(args.ipsw)}")
        cmd_prepare(argparse.Namespace(
            ipsw=args.ipsw, out=fw, keys=args.keys, skip_rootfs=False, redo_rootfs=False, keep_dmg=False,
            stock_lockdownd=args.stock_lockdownd, stock_jobs=args.stock_jobs, docs=None, cpus=1, nand="ppn",
            boot_args=None, verbose_boot=False, add_boot_arg=None, hide=None, ramdisk=None, ramdisk_file=None,
            bundle=None, seed=DEFAULT_SEED))
        return fw
    with open(os.path.join(fw, "hashes.json")) as f:
        want = json.load(f).get("ipsw", {}).get("sha256")
    log(f"firmware: {fw} is prepared; checking it is this IPSW's")
    have = sha256_file(args.ipsw)
    if want and want != have:
        raise SystemExit(f"{fw} was prepared from another IPSW (sha256 {want[:16]}..., this one {have[:16]}...): "
                         "pass --firmware DIR for a directory of its own")
    firmware_edits(os.path.join(fw, "rootfs.hfs"), args)
    return fw


DEVICE_CPUS = 2          # the A5's two Cortex-A9s; the install boot keeps to one


def device_kboot(dev, fw, meta, install, cpus=DEVICE_CPUS):
    """dev's bundle for the install boot (the install ramdisk as root, the FTL allowed to format a blank
    store, the AE2 hidden; one core) or for the normal boot (cpus cores), built for dev's geometry and
    with dev's identity."""
    return build_kboot(fw, argparse.Namespace(
        cpus=1 if install else cpus, nand=store_geometry(dev.nand), boot_args=None, verbose_boot=False, seed=meta["seed"],
        identity_dir=dev.dir, ipsw=None, keys=None,
        add_boot_arg=["nand-enable-reformat=1"] if install else None,
        hide=["arm-io/ae2"] if install else None,
        ramdisk="restore" if install else None,
        ramdisk_file=os.path.join(dev.install, "ramdisk-install.dmg") if install else None,
        bundle=os.path.join(dev.install, "kboot.bin") if install else dev.kboot))


def install_device(dev, fw, meta, args):
    """The install boot, start to finish: helpers and markers for this store, the install bundle, the boot
    until "it_install: done", then the normal bundle. The device is "ready" only after all of it."""
    import device
    fmt = store_geometry(dev.nand).get("format")
    if fmt != nand_ppn.PPN["format"]:
        raise SystemExit(f"device {dev.name!r}: its store is {fmt} (made before 2026-10-18); the install writes the "
                         f"data volume protected now, which only a {nand_ppn.PPN['format']} store keeps readable: "
                         f"device delete {dev.name}, then device create it again")
    meta["state"] = "installing"
    dev.save(meta)
    cmd_install(argparse.Namespace(dir=fw, nand=dev.nand, out=dev.install, ipsw=meta.get("ipsw"),
                                   keys=getattr(args, "keys", None), clang=args.clang, lld=args.lld,
                                   activation="factory"))
    device_kboot(dev, fw, meta, install=True)
    limit = args.limit or 1200 + 150 * meta["storage_gib"]
    log(f"install boot of {dev.name!r}: up to {limit} s (logs in {dev.logs})")
    result, secs = device.run_install_boot(dev, limit, log=log)
    meta.setdefault("installs", []).append({"at": time.strftime("%Y-%m-%d %H:%M:%S"), "seconds": round(secs),
                                            "result": result or f"no result in {limit} s"})
    if not result or device.DONE not in result:
        meta["state"] = "failed"
        dev.save(meta)
        raise SystemExit(f"install boot of {dev.name!r}: {result or 'no it_install result'} after {secs:.0f} s; "
                         f"serial log {os.path.join(dev.logs, 'install-serial.log')}")
    log(f"install boot: done in {secs:.0f} s")
    device_kboot(dev, fw, meta, install=False)
    meta["state"] = "ready"
    dev.save(meta)


def cmd_device_create(args):
    import device
    import secrets
    dev = device.Device(args.root, args.name)
    if dev.exists():
        raise SystemExit(f"device {args.name!r} already exists in {dev.dir} (device delete {args.name} first)")
    if os.path.isdir(dev.dir) and os.listdir(dev.dir):
        raise SystemExit(f"{dev.dir} exists and is not a device: pick another name or --root")
    gib = storage_gib(args.storage)
    ip = IPSW(args.ipsw)
    if ip.board.lower() != "n94ap":
        raise SystemExit(f"{os.path.basename(args.ipsw)}: {ip.product} ({ip.board}); devices are the iPhone 4S "
                         "(n94ap) for now")
    fw = ensure_firmware(args, ip)
    os.makedirs(dev.dir, exist_ok=True)
    meta = {"name": args.name, "created": time.strftime("%Y-%m-%d %H:%M:%S"), "product": ip.product,
            "board": ip.board, "version": ip.version, "build": ip.build, "firmware": fw,
            "ipsw": os.path.abspath(args.ipsw), "storage_gib": gib, "seed": f"device {secrets.token_hex(8)}",
            "state": "new"}
    dev.save(meta)
    g = nand_ppn.geometry(gib)
    nand_ppn.write_store(dev.nand, g)
    log(f"store: {dev.nand}, {gib} GiB raw ({g['blocks_per_cau']} blocks per CAU), sparse on the host")
    install_device(dev, fw, meta, args)
    log(f"device {args.name!r} is ready: ios_prep.py device boot {args.name}"
        + ("" if os.path.abspath(args.root) == os.path.abspath(device.default_root()) else f" --root {args.root}"))


def cmd_device_refresh(args):
    """The install boot again on an installed store: the markers rewritten (after a firmware edit, which
    pages the FTL had moved would not follow), the data volume and its keybag kept."""
    import device
    dev = device.Device(args.root, args.name)
    meta = dev.load()
    install_device(dev, meta["firmware"], meta, args)


def bundle_cpus(bundle):
    """The cores a bundle's device tree shows (its .json), 1 for a bundle made before it was recorded."""
    try:
        with open(os.path.splitext(bundle)[0] + ".json") as f:
            return int(json.load(f).get("cpus", 1))
    except (OSError, ValueError):
        return 1


def boot_logo_current(bundle, fw):
    """False when fw has a boot logo and bundle's .json does not record one (a bundle made before
    build_kboot drew it)."""
    if not os.path.exists(os.path.join(fw, "AppleLogo.bin")):
        return True
    try:
        with open(os.path.splitext(bundle)[0] + ".json") as f:
            return "boot_logo" in json.load(f)
    except (OSError, ValueError):
        return False


def cmd_device_boot(args):
    import device
    dev = device.Device(args.root, args.name)
    meta = dev.load()
    if meta.get("state") != "ready":
        raise SystemExit(f"device {args.name!r} is {meta.get('state')}, not ready: device refresh {args.name} "
                         "re-runs its install boot")
    if not boot_logo_current(dev.kboot, meta["firmware"]):
        log("kboot.bin predates the boot logo: building it again (same geometry and identity)")
        device_kboot(dev, meta["firmware"], meta, install=False, cpus=args.cpus)
    elif bundle_cpus(dev.kboot) != args.cpus:
        log(f"kboot.bin shows {bundle_cpus(dev.kboot)} core(s): building it again for {args.cpus}")
        device_kboot(dev, meta["firmware"], meta, install=False, cpus=args.cpus)
    extra = ["-smp", str(args.cpus)] + list(args.qemu_args)
    log(f"booting {args.name!r} ({meta['product']} {meta['version']} {meta['build']}, {meta['storage_gib']} GiB); "
        f"serial log {os.path.join(dev.logs, 'serial.log')}"
        + (f"; QMP on {args.qmp} and {args.qmp + 1} (tools/dev/n94_input.py --port)" if args.qmp else ""))
    rc = device.boot(dev, display=args.display, qmp=args.qmp, extra=extra, detach=args.detach, usb=not args.no_usb)
    log(f"QEMU pid {rc}, detached" if args.detach else f"QEMU exit {rc}")


def cmd_device_list(args):
    import device
    devs = device.list_devices(args.root)
    if not devs:
        print(f"no devices under {os.path.abspath(args.root)}")
        return
    print(f"{'NAME':20s} {'MODEL':10s} {'iOS':16s} {'STORAGE':8s} {'STATE':11s} {'QEMU':8s} PATH")
    for dev in devs:
        m = dev.load()
        pid = dev.running()
        print(f"{dev.name:20s} {m.get('product', '?'):10s} {m.get('version', '?') + ' ' + m.get('build', ''):16s} "
              f"{str(m.get('storage_gib', '?')) + ' GiB':8s} {m.get('state', '?'):11s} {str(pid or '-'):8s} {dev.dir}")


def cmd_device_delete(args):
    import device
    dev = device.Device(args.root, args.name)
    meta = dev.load()
    if not args.yes:
        if not sys.stdin.isatty():
            raise SystemExit(f"delete {args.name!r} ({dev.dir}): pass --yes to confirm")
        answer = input(f"delete device {args.name!r} ({meta.get('storage_gib')} GiB, {dev.dir})? [y/N] ")
        if answer.strip().lower() not in ("y", "yes"):
            raise SystemExit("left as it is")
    device.remove(dev)
    log(f"device {args.name!r} deleted ({dev.dir}); the firmware {meta.get('firmware')} stays for the others")


def add_device_parsers(sub):
    import device
    p = sub.add_parser("device", help="virtual devices: create, boot, refresh, list, delete (docs/n94/README.md)")
    dsub = p.add_subparsers(dest="device_cmd", required=True)

    def parser(name, help):
        q = dsub.add_parser(name, help=help)
        q.add_argument("--root", default=device.default_root(),
                       help="where the devices live (default: IPHONEMU_DEVICES, else devices/ beside the repository)")
        return q

    def install_opts(q):
        q.add_argument("--limit", type=int, help="seconds the install boot may take (default 1200 + 150 per GiB)")
        q.add_argument("--clang", help="clang for the install helpers (default: PATH, then the workspace UCRT64)")
        q.add_argument("--lld", help="ld.lld to use")

    q = parser("create", "a new device: its store, the install boot, ready to boot")
    q.add_argument("name")
    q.add_argument("--ipsw", required=True)
    q.add_argument("--storage", default="16G",
                   help="raw NAND size: " + ", ".join(f"{n}G" for n in nand_ppn.STORAGE_GIB) + " (default 16G; the "
                        "4S was sold with 16, 32 and 64 GB; the store is sparse and grows as the guest writes)")
    q.add_argument("--firmware", help="the prepared firmware directory (default: firmware/<board>_<build> beside "
                                      "the repository, prepared from the IPSW when not there)")
    q.add_argument("--keys", help="keys JSON (else cached beside the IPSW, else The Apple Wiki)")
    q.add_argument("--stock-lockdownd", action="store_true", help="as for prepare: no activation short-circuit")
    q.add_argument("--stock-jobs", action="store_true", help="as for prepare: no M3 launchd edits")
    install_opts(q)
    q.set_defaults(fn=cmd_device_create)
    q = parser("refresh", "the install boot again (markers rewritten, data volume kept)")
    q.add_argument("name")
    install_opts(q)
    q.set_defaults(fn=cmd_device_refresh)
    q = parser("boot", "boot a ready device")
    q.add_argument("name")
    q.add_argument("--display", default="sdl", help="QEMU -display (default sdl, the panel in a window; none)")
    q.add_argument("--qmp", type=int, default=4444,
                   help="QMP on this TCP port and the next (tools/dev/n94_input.py; default 4444, 0 for none)")
    q.add_argument("--detach", action="store_true", help="return once QEMU has started")
    q.add_argument("--no-usb", action="store_true",
                   help="no USB link to the host (by default tools/dev/usbmux_bridge.py serves usbmuxd on "
                        "127.0.0.1:27015 for libimobiledevice, the cable as a Mac's charging port)")
    q.add_argument("--cpus", type=int, choices=(1, 2), default=DEVICE_CPUS,
                   help=f"cores (default {DEVICE_CPUS}, as the A5; the bundle is rebuilt when it shows another count)")
    q.set_defaults(fn=cmd_device_boot)     # more QEMU arguments after a lone --
    q = parser("list", "the devices and their state")
    q.set_defaults(fn=cmd_device_list)
    q = parser("delete", "remove a device's directory (its store); the firmware stays")
    q.add_argument("name")
    q.add_argument("--yes", action="store_true", help="no confirmation question")
    q.set_defaults(fn=cmd_device_delete)


def add_kboot_opts(p):
    p.add_argument("--cpus", type=int, choices=(1, 2), default=1, help="cores visible to the kernel (default 1)")
    p.add_argument("--nand", choices=("raw", "ppn"), default="raw",
                   help="disk node geometry: raw A4-style NAND for the h2fmi model (default) or PPN")
    p.add_argument("--nand-store", metavar="DIR",
                   help="PPN with this page store's geometry.json (a store not of the default 8 GiB)")
    p.add_argument("--boot-args", help="kernel boot-args (default: %s)" % kboot.DEFAULT_BOOT_ARGS)
    p.add_argument("--verbose-boot", action="store_true", help="append -v (text console on the panel)")
    p.add_argument("--add-boot-arg", action="append", metavar="ARG",
                   help="append one boot-arg (repeatable), e.g. nand-enable-reformat=1 for the FTL's first format")
    p.add_argument("--hide", action="append", metavar="NODE", help="DT node to unmatch (repeatable)")
    p.add_argument("--ramdisk", choices=("restore", "update"),
                   help="stage the IPSW's restore or update ramdisk as the root (rd=md0): launchd with no NAND")
    p.add_argument("--ramdisk-file", metavar="IMAGE", help="stage this raw HFS+ image instead of the IPSW's ramdisk")
    p.add_argument("--bundle", metavar="FILE", help="write the bundle here instead of DIR/kboot.bin (and its .json)")
    p.add_argument("--seed", default=DEFAULT_SEED, help="identity seed (serial, ECID, MACs)")
    p.add_argument("--docs", help="also copy devicetree.txt/kexts.txt/iomap.tsv into this directory")
    p.add_argument("--no-boot-logo", action="store_true",
                   help="leave the panel black at reset (by default DIR/AppleLogo.bin is drawn on it, as iBoot does)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info")
    p.add_argument("ipsw")
    p.set_defaults(fn=cmd_info)
    p = sub.add_parser("prepare")
    p.add_argument("ipsw")
    p.add_argument("--out", required=True)
    p.add_argument("--keys", help="keys JSON (else cached beside the IPSW, else The Apple Wiki)")
    p.add_argument("--skip-rootfs", action="store_true")
    p.add_argument("--redo-rootfs", action="store_true")
    p.add_argument("--keep-dmg", action="store_true", help="keep the decrypted UDIF beside rootfs.hfs")
    p.add_argument("--stock-lockdownd", action="store_true",
                   help="leave lockdownd as the IPSW has it (by default it takes its activation short-circuit, "
                        "lockdownd_patch.py, so a new unit gets past Setup's activation page)")
    p.add_argument("--stock-jobs", action="store_true",
                   help="leave the launchd jobs as the IPSW has them (by default backboardd and SpringBoard get "
                        "CA_ENABLE_OGL=0 and BTServer/BlueTool are disabled, sysvol.UNIT_EDITS)")
    add_kboot_opts(p)
    p.set_defaults(fn=cmd_prepare)
    p = sub.add_parser("kboot")
    p.add_argument("dir")
    p.add_argument("--ipsw", help="the IPSW, needed once to decrypt a --ramdisk")
    p.add_argument("--keys", help="keys JSON (else cached beside the IPSW, else The Apple Wiki)")
    add_kboot_opts(p)
    p.set_defaults(fn=cmd_kboot)
    p = sub.add_parser("nand", help="write a blank PPN page store (geometry.json) for -M iPhone-4S,nand=DIR")
    p.add_argument("--out", required=True)
    p.add_argument("--storage", default="8G", help="raw size, " + ", ".join(f"{n}G" for n in nand_ppn.STORAGE_GIB)
                   + " (default 8G; another size needs a kboot.bin built with --nand-store)")
    p.add_argument("--ptab-only", action="store_true",
                   help="rewrite only the partition table (boot block) of an existing store, keeping its FTL")
    p.set_defaults(fn=cmd_nand)
    p = sub.add_parser("install", help="build the install boot: it_install, markers.json, the install ramdisk")
    p.add_argument("dir", help="the prepared firmware directory (rootfs.hfs, ramdisk-restore.dmg)")
    p.add_argument("--nand", required=True, help="the PPN page store directory (ios_prep.py nand --out)")
    p.add_argument("--out", required=True, help="where the helper and the install ramdisk go")
    p.add_argument("--ipsw", help="the IPSW, needed once to decrypt the restore ramdisk")
    p.add_argument("--keys")
    p.add_argument("--clang", help="clang to use (default: PATH, then the workspace MSYS2 UCRT64)")
    p.add_argument("--lld", help="ld.lld to use")
    p.add_argument("--activation", choices=("factory", "none"), default="factory",
                   help="seed lockdownd's data ark as a factory unit's (default), or leave lockdownd to make it "
                   "(Unactivated: Setup stops at its activation page)")
    p.set_defaults(fn=cmd_install)
    p = sub.add_parser("inventory")
    p.add_argument("dir")
    p.add_argument("--docs")
    p.set_defaults(fn=cmd_inventory)
    p = sub.add_parser("sysvol", help="edit launchd jobs on rootfs.hfs (the page store serves it to the guest)")
    p.add_argument("dir", help="the prepared firmware directory (rootfs.hfs)")
    p.add_argument("--job", action="append", default=[], metavar="LABEL",
                   help="a launchd job label (repeatable), e.g. com.apple.backboardd")
    p.add_argument("--console", action="store_true", help="the jobs' stdout and stderr to /dev/console")
    p.add_argument("--output", metavar="PATH", help="the jobs' stdout and stderr to this file in the guest instead "
                   "(on /private/var, readable back from the NAND pages)")
    p.add_argument("--env", action="append", default=[], metavar="KEY=VALUE",
                   help="an EnvironmentVariables entry for the jobs (repeatable), e.g. CA_ENABLE_OGL=0")
    p.add_argument("--program", nargs="+", metavar="ARG",
                   help="the jobs' ProgramArguments, a stock binary of the system volume (e.g. /bin/launchctl list: "
                        "launchd's job table, with PIDs and last exit statuses, on the console)")
    p.add_argument("--interval", type=int, default=0, metavar="SECS",
                   help="StartInterval: launchd re-runs the jobs every SECS seconds")
    p.add_argument("--disable", type=lambda s: s.lower() in ("1", "true", "yes", "on"), metavar="BOOL",
                   help="the plist's Disabled key (true: launchd neither loads the job nor registers its Mach "
                        "services; false: back on)")
    p.add_argument("--asl-console", action="store_true",
                   help="asl.conf: syslogd copies every message to /dev/console (the daemons' log on the serial console)")
    p.add_argument("--activation-shortcut", action="store_true",
                   help="lockdownd takes its activation short-circuit unconditionally (lockdownd_patch.py; 6.1.x "
                        "needs it, a seeded FactoryActivated does not hold there); fails closed on another binary")
    p.add_argument("--show", action="store_true", help="print the jobs as they are, change nothing")
    p.set_defaults(fn=cmd_sysvol)
    add_device_parsers(sub)
    argv, extra = sys.argv[1:], []
    if "--" in argv:
        argv, extra = argv[:argv.index("--")], argv[argv.index("--") + 1:]
    args = ap.parse_args(argv)
    args.qemu_args = extra
    args.fn(args)


if __name__ == "__main__":
    main()
