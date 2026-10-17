#!/usr/bin/env python3
"""ios-prep: turn an Apple IPSW into the kernel bundle the iPhonEmu machines boot.

  ios_prep.py info      IPSW                      what the IPSW is (device, build, components)
  ios_prep.py prepare   IPSW --out DIR [options]   decrypt kernel/DT/iBoot, expand the rootfs, build kboot.bin
  ios_prep.py kboot     DIR [options]              rebuild kboot.bin from a prepared directory
  ios_prep.py inventory DIR [--docs DOCS]          device-tree dump and the kernelcache's kext list

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


def build_kboot(out_dir, args):
    kernel = read(os.path.join(out_dir, "kernelcache.mach"))
    dtb = read(os.path.join(out_dir, "DeviceTree.bin"))
    iboot_path = os.path.join(out_dir, "iBoot.bin")
    iboot = read(iboot_path) if os.path.exists(iboot_path) else None
    board = kboot.board_of(DeviceTree(dtb))
    ident = load_identity(out_dir, args.seed, board)
    boot_args = args.boot_args or kboot.DEFAULT_BOOT_ARGS
    if args.verbose_boot and "-v" not in boot_args.split():
        boot_args += " -v"
    image, load_pa, entry, ba_pa, info = kboot.build(kernel, dtb, ident, boot_args=boot_args, iboot=iboot,
                                                      cpus=args.cpus, nand=args.nand, hide=args.hide or ())
    blob = kboot.bundle(image, load_pa, entry, ba_pa)
    out = os.path.join(out_dir, "kboot.bin")
    with open(out, "wb") as f:
        f.write(blob)
    info["kboot"] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
    with open(os.path.join(out_dir, "kboot.json"), "w") as f:
        json.dump(info, f, indent=2, default=lambda v: "0x%x" % v if isinstance(v, int) else v)
    log(f"kboot.bin: {len(blob)} bytes, load 0x{load_pa:08x} entry 0x{entry:08x} boot_args 0x{ba_pa:08x} "
        f"(Version {info['boot_args_version']}, {args.cpus} cpu(s), nand={args.nand})")
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
    write_inventory(out, args.docs)
    build_kboot(out, args)
    for name in ("kernelcache.mach", "DeviceTree.bin", "iBoot.bin", "kboot.bin", "rootfs.hfs"):
        p = os.path.join(out, name)
        if os.path.exists(p):
            hashes[name] = {"bytes": os.path.getsize(p), "sha256": sha256_file(p)}
    with open(os.path.join(out, "hashes.json"), "w") as f:
        json.dump(hashes, f, indent=2)
    log("done")


def cmd_kboot(args):
    build_kboot(args.dir, args)


def cmd_inventory(args):
    rows, kexts, ver = write_inventory(args.dir, args.docs)
    print(ver)
    for r in rows:
        print("%-22s %-40s %s  irq %s" % r)


def add_kboot_opts(p):
    p.add_argument("--cpus", type=int, choices=(1, 2), default=1, help="cores visible to the kernel (default 1)")
    p.add_argument("--nand", choices=("raw", "ppn"), default="raw",
                   help="disk node geometry: raw A4-style NAND for the h2fmi model (default) or PPN")
    p.add_argument("--boot-args", help="kernel boot-args (default: %s)" % kboot.DEFAULT_BOOT_ARGS)
    p.add_argument("--verbose-boot", action="store_true", help="append -v (text console on the panel)")
    p.add_argument("--hide", action="append", metavar="NODE", help="DT node to unmatch (repeatable)")
    p.add_argument("--seed", default="iPhonEmu n94 dev1", help="identity seed (serial, ECID, MACs)")
    p.add_argument("--docs", help="also copy devicetree.txt/kexts.txt/iomap.tsv into this directory")


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
    add_kboot_opts(p)
    p.set_defaults(fn=cmd_prepare)
    p = sub.add_parser("kboot")
    p.add_argument("dir")
    add_kboot_opts(p)
    p.set_defaults(fn=cmd_kboot)
    p = sub.add_parser("inventory")
    p.add_argument("dir")
    p.add_argument("--docs")
    p.set_defaults(fn=cmd_inventory)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
