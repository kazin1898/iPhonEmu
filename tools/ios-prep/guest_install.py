"""The install boot's pieces: it_install (contrib/it-install) built as a static armv7 Mach-O, its
install table, the page store's markers.json and a restore ramdisk with the helper in it.

The 4S kernel has only AppleSwissPPNFTL, whose on-flash format is undocumented, so the system
volume cannot be written into the page store offline. Instead the IPSW's restore ramdisk boots once
with it_install in place of restored_external (the ramdisk's rc.boot execs the first of
restored_external, restored_update, restored and ramrod; the fork's it_keybag takes the same slot
on the A4 boards). The helper writes *marker* pages through /dev/rdisk0 (the FTL's block device) and
the FTL lays them out however it likes. A marker is one 4 KiB logical page:

    +0   16 bytes  MARKER_MAGIC
    +16  u32       source index (markers.json "sources")
    +20  u32       4 KiB block in that source
    +24  u32       source ^ block ^ MARKER_CHECK
    +28  zeros to the end of the page

and the page store (hw/arm/s5l8940_iop.c) serves every stored marker page from the host file it
names, so the guest's FTL holds the whole volume while the host keeps the bytes. Pages the guest
rewrites later are ordinary pages.

Building the helper: the MSYS2 clang compiles it_install.c as a bare-metal armv7 ELF object
(--target=armv7-none-eabi, ARM mode, no libc, no PIC), ld.lld links it at the Mach-O's __TEXT
address with a linker script that keeps _start first and refuses writable data, and the ELF's
one loadable segment is wrapped here in a static Mach-O (MH_EXECUTE, __PAGEZERO, __TEXT,
LC_UNIXTHREAD; no dyld, no code signature: the boot-args carry the fork's
amfi_allow_any_signature=1 and cs_enforcement_disable=1). The fork builds its helpers with an iOS
SDK and ld64 (contrib/armv6-toolchain), which this Windows host does not have; the wrapper is
about 80 lines and keeps the build self-contained.

it_keybag (contrib/it-install/it_keybag.c, the B4 step) is built the same way but as a dyld-linked
Mach-O: it calls the ramdisk's own IOKit and MobileKeyBag through dlopen/dlsym, so its Mach-O has a
__DATA segment with the three dl* pointer slots, a __LINKEDIT with the bind opcodes dyld follows to
fill them from /usr/lib/libSystem.B.dylib (LC_DYLD_INFO_ONLY), LC_LOAD_DYLINKER /usr/lib/dyld and
LC_LOAD_DYLIB. it_install execs it with the data volume mounted at /mnt2.
"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import hfs

MARKER_MAGIC = b"iPhonEmu-marker1"
MARKER_CHECK = 0xA5000000
BLOCK = 4096
SYSTEM_LBA = 1              # the MBR takes block 0

PAGEZERO = 0x4000
HEADER_ROOM = 0x1000        # the Mach-O header's page before the code
TEXT_VM = PAGEZERO
CODE_VM = TEXT_VM + HEADER_ROOM

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "contrib", "it-install", "it_install.c")
KEYBAG_SRC = os.path.join(HERE, "..", "..", "contrib", "it-install", "it_keybag.c")
KEYBAG_SLOT = "/usr/bin/TiSerialFlasher"     # an existing ramdisk file it_keybag replaces (hfs.py edits in place;
                                           # the touch-controller flasher is of no use in the emulator)
KEYBAG_IMPORTS = {"it_dlopen": "_dlopen", "it_dlsym": "_dlsym", "it_dlerror": "_dlerror"}
LIBSYSTEM = "/usr/lib/libSystem.B.dylib"
DYLD = "/usr/lib/dyld"

CFLAGS = ["--target=armv7-none-eabi", "-marm", "-mcpu=cortex-a9", "-mfloat-abi=soft", "-O2",
          "-ffreestanding", "-fno-builtin", "-nostdlib", "-fno-stack-protector", "-fno-pic",
          "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-fno-jump-tables", "-Wall", "-Wextra"]

LINKER_SCRIPT = """ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); }
SECTIONS {
  . = 0x%x;
  .text : { KEEP(*(.text.start)) *(.text .text.*) } :text
  .rodata : { *(.rodata .rodata.*) } :text
  .data : { *(.data .data.*) } :text
  .bss : { *(.bss .bss.* COMMON) } :text
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) *(.note*) }
}
"""

# The dyld-linked helper: the code as above, then a writable segment on the next page (its first
# bytes the dl* slots dyld binds), so the Mach-O gets __TEXT and __DATA at the same addresses.
LINKER_SCRIPT_DYLD = """ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = 0x%x;
  .text : { KEEP(*(.text.start)) *(.text .text.*) } :text
  .rodata : { *(.rodata .rodata.*) } :text
  . = ALIGN(0x1000);
  .data : { KEEP(*(.data.binds)) *(.data .data.*) } :data
  .bss : { *(.bss .bss.* COMMON) } :data
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) *(.note*) }
}
"""


def marker(source, block):
    return (MARKER_MAGIC + struct.pack("<III", source, block, source ^ block ^ MARKER_CHECK)).ljust(BLOCK, b"\0")


def c_string(b):
    return '"' + "".join("\\x%02x" % c for c in b) + '"'


VAR_FILE_MAX = 4096     # files of the skeleton bigger than this are left out (the PreinstalledAssets cpios)


def var_skeleton(rootfs):
    """The system volume's /private/var as a restore lays it on the data volume: [(kind, path, mode, uid,
    gid, data)] with kind 'd' (folder), 'f' (file, its bytes) or 'l' (symlink, its target), paths
    relative to /private/var, parents first. 6.x's daemons expect it (the photo library's CoreData store
    raised "Error validating url for store" without mobile/Media/PhotoData), and the helper's own list
    had only part of it. Files over VAR_FILE_MAX bytes are skipped."""
    v = hfs.Volume(rootfs)
    try:
        top = v.lookup("/private/var")
        kids = {}
        for (parent, name), val in v.index().items():
            kids.setdefault(parent, []).append((name, val))
        out = []

        def walk(fid, path):
            for name, (at, off, rtype, rlen) in sorted(kids.get(fid, [])):
                rec = bytes(v.catalog[at:at + rlen])
                uid, gid = struct.unpack_from(">II", rec, off + 32)
                mode = struct.unpack_from(">H", rec, off + 42)[0]
                p = path + "/" + name
                if rtype == hfs.REC_FOLDER:
                    out.append(("d", p, mode & 0o7777, uid, gid, b""))
                    walk(struct.unpack_from(">I", rec, off + 8)[0], p)
                    continue
                kind = "l" if mode & 0o170000 == 0o120000 else "f"
                size = struct.unpack_from(">Q", rec, off + 88)[0]
                if kind == "f" and size > VAR_FILE_MAX:
                    continue
                out.append((kind, p, mode & 0o7777, uid, gid, v.read_file("/private/var" + p)))
        walk(top, "")
        return out
    finally:
        v.close()


# lockdownd's data ark as a factory unit carries it (docs/n81/README.md "iOS 6", item 4): the activation
# state cached as FactoryActivated and the brick state off. Keys are "<domain>-<key>", the empty domain
# for BrickState. 6.x lockdownd stays stock and reads its state from here.
DATA_ARK = b"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
\t<key>-BrickState</key>
\t<false/>
\t<key>com.apple.mobile.lockdown_cache-ActivationState</key>
\t<string>FactoryActivated</string>
</dict>
</plist>
"""


def lockdown_seed(skeleton):
    """The data ark entries for the skeleton: /private/var/root/Library/Lockdown/data_ark.plist (root,
    0644) and its folder, the folders the IPSW does not have added parents first. The install boot writes
    it only where there is none (as every skeleton file): once lockdownd has run, the data ark is its own
    record of the unit, and a refresh that put the seed back sent a unit past Setup to Setup's update
    pages. (With lockdownd's activation short-circuit it cannot turn Unactivated, which the rewrite was
    for.)"""
    have = {p for k, p, *_ in skeleton if k == "d"}
    out = [("d", d, 0o755, 0, 0, b"") for d in ("/root", "/root/Library", "/root/Library/Lockdown")
           if d not in have]
    return out + [("f", "/root/Library/Lockdown/data_ark.plist", 0o644, 0, 0, DATA_ARK)]


def table_h(system_blocks, system_source=0, system_lba=SYSTEM_LBA, skeleton=()):
    """it_install_table.h: what the helper writes, for one system volume, and the /private/var skeleton
    (VAR_SKELETON entries {kind, mode, uid, gid, path, data, size})."""
    rows = "".join("    { '%s', 0%o, %u, %u, %s, %s, %u }, \\\n"
                   % (k, mode, uid, gid, c_string(p.encode()), c_string(data), len(data))
                   for k, p, mode, uid, gid, data in skeleton)
    return ("/* Generated by tools/ios-prep/guest_install.py: the install table it_install.c writes. */\n"
            "#define MARKER_MAGIC %s\n"
            "#define MARKER_CHECK 0x%08Xu\n"
            "#define SYSTEM_SOURCE %uu\n"
            "#define SYSTEM_LBA %uu\n"
            "#define SYSTEM_BLOCKS %uu\n"
            "#define VAR_SKELETON \\\n%s\n"
            % (c_string(MARKER_MAGIC), MARKER_CHECK, system_source, system_lba, system_blocks, rows))


def markers_json(sources):
    """The page store's markers.json: {"magic", "check", "sources": [{"path", "bytes", "sha256"?}]}.
    Paths are kept as given (absolute, or relative to the store directory)."""
    return {"format": "iphonemu-markers-v1", "magic": MARKER_MAGIC.decode(), "check": MARKER_CHECK,
            "block": BLOCK, "sources": sources}


# ---- a small ELF32 reader ----------------------------------------------------------------------------

def elf_segments(path):
    """(entry, [(vaddr, flags, file bytes, memsz)]) of an ELF32 LE's PT_LOAD segments, and the ELF."""
    with open(path, "rb") as f:
        e = f.read()
    if e[:4] != b"\x7fELF" or e[4] != 1 or e[5] != 1:
        raise ValueError(f"{path}: not an ELF32 LE")
    e_entry, e_phoff = struct.unpack_from("<II", e, 0x18)
    e_phentsize, e_phnum = struct.unpack_from("<HH", e, 0x2a)
    segs = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, p_flags, _ = struct.unpack_from(
            "<8I", e, e_phoff + i * e_phentsize)
        if p_type == 1 and p_memsz:
            segs.append((p_vaddr, p_flags, e[p_offset:p_offset + p_filesz], p_memsz))
    if not segs:
        raise ValueError(f"{path}: no loadable segment")
    return e_entry, segs, e


def elf_symbols(e):
    """{name: value} from an ELF32 LE's .symtab (for the dyld helper's import slots)."""
    e_shoff = struct.unpack_from("<I", e, 0x20)[0]
    e_shentsize, e_shnum = struct.unpack_from("<HH", e, 0x2e)
    sections = [struct.unpack_from("<10I", e, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    syms = {}
    for sh in sections:
        if sh[1] != 2:                                   # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 16):
            st_name, st_value = struct.unpack_from("<II", e, off)
            end = e.index(b"\0", strtab[4] + st_name)
            syms[e[strtab[4] + st_name:end].decode()] = st_value
    return syms


def elf_load_image(path):
    """(entry, lowest vaddr, bytes) of an ELF32 LE's PT_LOAD segments, laid out at their addresses;
    raises if any loadable segment is writable (the static Mach-O wrapper maps one read-only segment)."""
    e_entry, segs, _ = elf_segments(path)
    for v, flags, _, memsz in segs:
        if flags & 2:
            raise ValueError(f"{path}: a writable segment at 0x{v:x} ({memsz} bytes); the helper "
                             "must have no .data/.bss")
    lo = min(v for v, _, _, _ in segs)
    hi = max(v + m for v, _, _, m in segs)
    image = bytearray(hi - lo)
    for v, _, data, _ in segs:
        image[v - lo:v - lo + len(data)] = data
    return e_entry, lo, bytes(image)


# ---- the Mach-O wrapper --------------------------------------------------------------------------------

MH_MAGIC = 0xFEEDFACE
CPU_TYPE_ARM = 12
CPU_SUBTYPE_ARM_V7 = 9
MH_EXECUTE = 2
MH_NOUNDEFS, MH_DYLDLINK, MH_TWOLEVEL = 1, 4, 0x80
LC_SEGMENT = 1
LC_SYMTAB = 2
LC_UNIXTHREAD = 5
LC_DYSYMTAB = 0xb
LC_LOAD_DYLIB = 0xc
LC_LOAD_DYLINKER = 0xe
LC_DYLD_INFO_ONLY = 0x80000022
VM_PROT_READ, VM_PROT_WRITE, VM_PROT_EXECUTE = 1, 2, 4
ARM_THREAD_STATE = 1
BIND_OPCODE_DONE = 0x00
BIND_OPCODE_SET_DYLIB_ORDINAL_IMM = 0x10
BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM = 0x40
BIND_OPCODE_SET_TYPE_IMM = 0x50
BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x70
BIND_OPCODE_DO_BIND = 0x90
BIND_TYPE_POINTER = 1
PAGE = 0x1000


def page_round(n):
    return (n + PAGE - 1) & ~(PAGE - 1)


def segment_cmd(name, vmaddr, vmsize, fileoff, filesize, maxprot, initprot):
    return struct.pack("<II16sIIIIIIII", LC_SEGMENT, 56, name.encode(), vmaddr, vmsize, fileoff, filesize,
                       maxprot, initprot, 0, 0)


def string_cmd(cmd, head, s):
    """A load command whose payload is `head` (packed) then a C string, padded to 8 bytes."""
    body = head + s.encode() + b"\0"
    size = (8 + len(body) + 7) & ~7
    return struct.pack("<II", cmd, size) + body.ljust(size - 8, b"\0")


def uleb(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def thread_cmd(entry):
    # ARM_THREAD_STATE: r0-r12, sp, lr, pc, cpsr. sp 0 = the kernel's default stack; cpsr 0x10 = ARM mode.
    regs = [0] * 13 + [0, 0, entry, 0x10]
    return struct.pack("<IIII", LC_UNIXTHREAD, 16 + 4 * len(regs), ARM_THREAD_STATE, len(regs)) + \
        struct.pack("<%dI" % len(regs), *regs)


def dyld_macho(code, code_vm, entry, data, data_vm, data_memsz, binds, dylib=LIBSYSTEM):
    """A dyld-linked armv7 Mach-O executable: `code` at code_vm (header on the page before), the writable
    `data` (memsz bytes, zero-filled past the file bytes) at data_vm, and `binds` = [(slot address,
    symbol)] bound by dyld from `dylib` through LC_DYLD_INFO bind opcodes. Not PIE: the segments are
    mapped at their link addresses, so the bare-metal code's absolute addresses hold."""
    if code_vm != CODE_VM:
        raise ValueError("the code must be linked at 0x%x" % CODE_VM)
    text_size = page_round(HEADER_ROOM + len(code))
    if data_vm != TEXT_VM + text_size:
        raise ValueError("the data must follow the code on the next page (0x%x, linked at 0x%x)"
                         % (TEXT_VM + text_size, data_vm))
    data_size = page_round(max(data_memsz, len(data), 1))
    linkedit_vm = data_vm + data_size
    # the bind opcodes: one dylib, pointer binds, each slot by its offset in __DATA (segment index 2)
    ops = bytearray([BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | 1, BIND_OPCODE_SET_TYPE_IMM | BIND_TYPE_POINTER])
    for addr, sym in binds:
        if not data_vm <= addr < data_vm + data_size:
            raise ValueError(f"bind slot 0x{addr:x} for {sym} is outside __DATA")
        ops += bytes([BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 2]) + uleb(addr - data_vm)
        ops += bytes([BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM]) + sym.encode() + b"\0"
        ops.append(BIND_OPCODE_DO_BIND)
    ops.append(BIND_OPCODE_DONE)
    ops = bytes(ops).ljust((len(ops) + 7) & ~7, b"\0")
    strtab = b"\0\0\0\0"                              # an empty symbol table: dyld binds by LC_DYLD_INFO
    linkedit = ops + strtab
    linkedit_off = text_size + data_size
    cmds = segment_cmd("__PAGEZERO", 0, PAGEZERO, 0, 0, 0, 0)
    cmds += segment_cmd("__TEXT", TEXT_VM, text_size, 0, text_size, VM_PROT_READ | VM_PROT_EXECUTE,
                        VM_PROT_READ | VM_PROT_EXECUTE)
    cmds += segment_cmd("__DATA", data_vm, data_size, text_size, data_size, VM_PROT_READ | VM_PROT_WRITE,
                        VM_PROT_READ | VM_PROT_WRITE)
    cmds += segment_cmd("__LINKEDIT", linkedit_vm, page_round(len(linkedit)), linkedit_off, len(linkedit),
                        VM_PROT_READ, VM_PROT_READ)
    cmds += struct.pack("<12I", LC_DYLD_INFO_ONLY, 48, 0, 0, linkedit_off, len(ops), 0, 0, 0, 0, 0, 0)
    cmds += struct.pack("<6I", LC_SYMTAB, 24, linkedit_off + len(ops), 0, linkedit_off + len(ops), len(strtab))
    cmds += struct.pack("<20I", LC_DYSYMTAB, 80, *([0] * 18))
    cmds += string_cmd(LC_LOAD_DYLINKER, struct.pack("<I", 12), DYLD)
    cmds += string_cmd(LC_LOAD_DYLIB, struct.pack("<IIII", 24, 2, 0x10000, 0x10000), dylib)
    cmds += thread_cmd(entry)
    ncmds = 4 + 3 + 2 + 1
    header = struct.pack("<7I", MH_MAGIC, CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, MH_EXECUTE, ncmds, len(cmds),
                         MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL)
    blob = header + cmds
    if len(blob) > HEADER_ROOM:
        raise ValueError("Mach-O header too large")
    return ((blob.ljust(HEADER_ROOM, b"\0") + code).ljust(text_size, b"\0") + data.ljust(data_size, b"\0")
            + linkedit)


def static_macho(code, code_vm, entry):
    """A static armv7 Mach-O executable mapping `code` at code_vm (its header on the page before)."""
    if code_vm != CODE_VM:
        raise ValueError("the code must be linked at 0x%x" % CODE_VM)
    text_size = (HEADER_ROOM + len(code) + 0xfff) & ~0xfff
    cmds = segment_cmd("__PAGEZERO", 0, PAGEZERO, 0, 0, 0, 0)
    cmds += segment_cmd("__TEXT", TEXT_VM, text_size, 0, text_size, VM_PROT_READ | VM_PROT_EXECUTE,
                        VM_PROT_READ | VM_PROT_EXECUTE)
    cmds += thread_cmd(entry)
    header = struct.pack("<7I", MH_MAGIC, CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, MH_EXECUTE, 3, len(cmds), MH_NOUNDEFS)
    blob = header + cmds
    if len(blob) > HEADER_ROOM:
        raise ValueError("Mach-O header too large")
    return (blob.ljust(HEADER_ROOM, b"\0") + code).ljust(text_size, b"\0")


# ---- the build -----------------------------------------------------------------------------------------

def find_tool(name, hint=None):
    """clang/ld.lld: $IT_CLANG-style override, the PATH, then the workspace's MSYS2 UCRT64."""
    if hint and os.path.exists(hint):
        return hint
    p = shutil.which(name)
    if p:
        return p
    for root in (os.path.join(HERE, "..", "..", "..", "toolchain", "msys64", "ucrt64", "bin"),):
        cand = os.path.join(root, name + (".exe" if sys.platform == "win32" else ""))
        if os.path.exists(cand):
            return os.path.abspath(cand)
    raise SystemExit(f"{name} not found (install the MSYS2 UCRT64 clang and lld, or set --clang/--lld)")


def compile_link(src, out_dir, name, script_text, clang=None, lld=None, keep=False, include=None):
    """Compile one helper source with the bare-metal clang and link it with ld.lld under `script_text`;
    returns (ELF path, clang path). The object is removed unless `keep`."""
    os.makedirs(out_dir, exist_ok=True)
    clang = find_tool("clang", clang)
    lld = find_tool("ld.lld", lld)
    obj = os.path.join(out_dir, name + ".o")
    elf = os.path.join(out_dir, name + ".elf")
    script = os.path.join(out_dir, name + ".ld")
    with open(script, "w") as f:
        f.write(script_text)
    extra = os.environ.get("IT_INSTALL_CFLAGS", "").split()      # e.g. -DIT_PROBE for the diagnostics build
    inc = ["-I", include] if include else []
    subprocess.run([clang] + CFLAGS + extra + inc + ["-c", os.path.abspath(src), "-o", obj], check=True)
    subprocess.run([lld, "-m", "armelf", "--no-dynamic-linker", "-nostdlib", "-static", "-T", script, "-o", elf, obj],
                   check=True)
    if not keep:
        os.remove(obj)
    return elf, clang


def build_helper(system_blocks, out_dir, clang=None, lld=None, src=SRC, keep=False, skeleton=()):
    """Compile it_install.c for this table into out_dir/it_install (the static Mach-O); returns its path
    and a dict with sizes and hashes. it_install_table.h and the ELF are written beside it."""
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "it_install_table.h"), "w") as f:
        f.write(table_h(system_blocks, skeleton=skeleton))
    elf, clang = compile_link(src, out_dir, "it_install", LINKER_SCRIPT % CODE_VM, clang, lld, keep, out_dir)
    entry, lo, code = elf_load_image(elf)
    if lo != CODE_VM or entry != CODE_VM:
        raise SystemExit(f"it_install: linked at 0x{lo:x} entry 0x{entry:x}, expected _start first at 0x{CODE_VM:x}")
    macho = static_macho(code, lo, entry)
    out = os.path.join(out_dir, "it_install")
    with open(out, "wb") as f:
        f.write(macho)
    info = {"source": rel_path(src, out_dir), "code_bytes": len(code), "macho_bytes": len(macho),
            "entry": "0x%x" % entry, "sha256": hashlib.sha256(macho).hexdigest(),
            "system_blocks": system_blocks, "system_lba": SYSTEM_LBA, "clang": clang,
            "var_skeleton": len(skeleton)}
    return out, info


def build_keybag_helper(out_dir, clang=None, lld=None, src=KEYBAG_SRC, keep=False):
    """Compile it_keybag.c into out_dir/it_keybag, the dyld-linked Mach-O (its dl* slots bound from
    libSystem); returns its path and a dict with sizes, the binds and hashes."""
    elf, clang = compile_link(src, out_dir, "it_keybag", LINKER_SCRIPT_DYLD % CODE_VM, clang, lld, keep)
    entry, segs, e = elf_segments(elf)
    text = [s for s in segs if not s[1] & 2]
    data = [s for s in segs if s[1] & 2]
    if len(text) != 1 or len(data) != 1 or text[0][0] != CODE_VM or entry != CODE_VM:
        raise SystemExit(f"it_keybag: expected one code segment at 0x{CODE_VM:x} (entry there) and one data "
                         f"segment, got {[(hex(v), f, m) for v, f, _, m in segs]}")
    syms = elf_symbols(e)
    missing = [n for n in KEYBAG_IMPORTS if n not in syms]
    if missing:
        raise SystemExit(f"it_keybag: import slots not in the ELF symbol table: {missing}")
    binds = [(syms[slot], sym) for slot, sym in KEYBAG_IMPORTS.items()]
    macho = dyld_macho(text[0][2], text[0][0], entry, data[0][2], data[0][0], data[0][3], binds)
    out = os.path.join(out_dir, "it_keybag")
    with open(out, "wb") as f:
        f.write(macho)
    info = {"source": rel_path(src, out_dir), "code_bytes": len(text[0][2]), "data_bytes": data[0][3],
            "macho_bytes": len(macho), "entry": "0x%x" % entry, "dyld": DYLD, "dylib": LIBSYSTEM,
            "binds": {sym: "0x%x" % addr for addr, sym in binds}, "sha256": hashlib.sha256(macho).hexdigest(),
            "slot": KEYBAG_SLOT, "clang": clang}
    return out, info


# ---- the ramdisk ---------------------------------------------------------------------------------------

def make_install_ramdisk(stock, out, helpers, grow_blocks=512):
    """A private copy of the restore ramdisk with each of `helpers` = [(slot path, local file)] in it (the
    stock ramdisk is 100% full, so the copy is grown by grow_blocks allocation blocks first). The first
    slot is where the ramdisk's rc.boot hands over (/usr/local/bin/restored_external)."""
    shutil.copyfile(stock, out)
    files = [(slot, open(path, "rb").read()) for slot, path in helpers]
    v = hfs.Volume(out, writable=True)
    try:
        need = sum(-(-len(data) // v.block_size) for _, data in files)
        if v.free_blocks < need + 8 * len(files):
            v.grow(v.total_blocks + max(grow_blocks, need + 8 * len(files)))
        placed = {}
        for slot, data in files:
            first, n = v.set_file_data(slot, data, mode=0o755)
            placed[slot] = [first, n]
    finally:
        v.close()
    return {"ramdisk": out, "bytes": os.path.getsize(out), "helpers": placed, "slot": helpers[0][0],
            "sha256": hashlib.sha256(open(out, "rb").read()).hexdigest()}


def rel_path(path, start):
    """path relative to start, or absolute when they are on different drives (Windows): a device's store
    may be on another drive than the firmware it serves (the model takes either)."""
    try:
        return os.path.relpath(os.path.abspath(path), os.path.abspath(start))
    except ValueError:
        return os.path.abspath(path)


def write_markers(store_dir, rootfs, extra_sources=()):
    """markers.json in the page store directory: source 0 is the system volume image."""
    size = os.path.getsize(rootfs)
    if size % BLOCK:
        raise SystemExit(f"{rootfs}: {size} bytes is not a whole number of 4 KiB blocks")
    rel = rel_path(rootfs, store_dir).replace(os.sep, "/")
    sources = [{"path": rel, "bytes": size, "what": "system volume (HFSX)"}] + list(extra_sources)
    path = os.path.join(store_dir, "markers.json")
    with open(path, "w") as f:
        json.dump(markers_json(sources), f, indent=1)
    return path, size // BLOCK


if __name__ == "__main__":
    # A quick self-check: build the helper for a 100-block volume into a temporary directory.
    d = tempfile.mkdtemp()
    out, info = build_helper(100, d, keep=True)
    print(json.dumps(info, indent=1))
    out, info = build_keybag_helper(d, keep=True)
    print(json.dumps(info, indent=1))
