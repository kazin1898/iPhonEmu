"""One armv7 Mach-O object -> an iOS dylib dyld loads, with no Apple linker.

contrib/gles-public's build.sh links the GL front end with ld64 and an iOS 3.x SDK, which exist only on
macOS. LLVM's ld64.lld has no armv7 (`-arch armv7`: "missing or unsupported"), so on Windows this does the
link: clang's MH_OBJECT in, an MH_DYLIB out that iOS 5/6's dyld (dyld-212 on 10B329) loads:

  __TEXT     the header and load commands, the object's __TEXT sections, then one stub per external
             function a `bl`/`b` reaches (ldr ip; add ip, pc, ip; ldr pc, [ip]: ARM, interworking)
  __DATA     the object's __DATA sections (zero-fill last), then __got: one bound pointer per stub target
  __LINKEDIT LC_DYLD_INFO_ONLY's rebase opcodes (every absolute pointer to the image), bind opcodes (every
             pointer to an import), the export trie (exactly the names asked for), then a symbol table of the
             exports and the imports for debuggers and dladdr

Imports are bound with the flat-lookup ordinal (BIND_SPECIAL_DYLIB_FLAT_LOOKUP): the libraries named with
`dylibs` are loaded (LC_LOAD_DYLIB) and a symbol is found in whichever loaded image exports it, so the link
needs no stub library saying which one does (NSObject moved from CoreFoundation to libobjc between
firmwares). Everything is bound at load; nothing is lazy.

Relocations handled are what clang emits for `-marm -mno-movt` PIC code: ARM_RELOC_BR24 (local or to a
stub), ARM_RELOC_VANILLA (data pointers: rebase, or bind with the addend in place), ARM_RELOC_SECTDIFF /
LOCAL_SECTDIFF + PAIR (literal pools: A - B), and the object's non-lazy pointer section entries (indirect
symbols: bound, or rebased for a local). Anything else fails the link by name; a pointer in __TEXT
(a text relocation) fails too.

    python -I macho_dylib.py OBJ OUT --install-name NAME --exports LIST [--dylib PATH ...]
    python -I macho_dylib.py --dump FILE        # sections, relocation kinds, imports (object or dylib)
"""
import argparse
import hashlib
import struct
import sys

MH_MAGIC = 0xFEEDFACE
CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7 = 12, 9
MH_OBJECT, MH_DYLIB = 1, 6
MH_NOUNDEFS, MH_DYLDLINK, MH_TWOLEVEL, MH_NO_REEXPORTED_DYLIBS = 0x1, 0x4, 0x80, 0x100000

LC_SEGMENT, LC_SYMTAB, LC_DYSYMTAB = 0x1, 0x2, 0xB
LC_LOAD_DYLIB, LC_ID_DYLIB = 0xC, 0xD
LC_UUID, LC_VERSION_MIN_IPHONEOS, LC_DYLD_INFO_ONLY = 0x1B, 0x25, 0x80000022

S_TYPE = 0xFF
S_ZEROFILL, S_NON_LAZY_SYMBOL_POINTERS, S_LAZY_SYMBOL_POINTERS, S_SYMBOL_STUBS = 0x1, 0x6, 0x7, 0x8
S_GB_ZEROFILL, S_THREAD_LOCAL_ZEROFILL = 0xC, 0x12
S_ATTR_PURE_INSTRUCTIONS, S_ATTR_SOME_INSTRUCTIONS = 0x80000000, 0x400
INDIRECT_SYMBOL_LOCAL, INDIRECT_SYMBOL_ABS = 0x80000000, 0x40000000

N_EXT, N_TYPE, N_UNDF, N_SECT, N_STAB = 0x01, 0x0E, 0x0, 0xE, 0xE0

ARM_RELOC_VANILLA, ARM_RELOC_PAIR, ARM_RELOC_SECTDIFF, ARM_RELOC_LOCAL_SECTDIFF = 0, 1, 2, 3
ARM_RELOC_BR24 = 5
RELOC_NAMES = {0: "VANILLA", 1: "PAIR", 2: "SECTDIFF", 3: "LOCAL_SECTDIFF", 4: "PB_LA_PTR", 5: "BR24",
               6: "THUMB_BR22", 7: "THUMB_32BIT_BRANCH", 8: "HALF", 9: "HALF_SECTDIFF"}

REBASE_TYPE_POINTER = 1
REBASE_OPCODE_SET_TYPE_IMM = 0x10
REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x20
REBASE_OPCODE_DO_REBASE_IMM_TIMES = 0x50
REBASE_OPCODE_DONE = 0x00
BIND_TYPE_POINTER = 1
BIND_SPECIAL_DYLIB_FLAT_LOOKUP = -2
BIND_OPCODE_SET_DYLIB_SPECIAL_IMM = 0x30
BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM = 0x40
BIND_OPCODE_SET_TYPE_IMM = 0x50
BIND_OPCODE_SET_ADDEND_SLEB = 0x60
BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x70
BIND_OPCODE_DO_BIND = 0x90
BIND_OPCODE_DONE = 0x00

PAGE = 0x1000
# ldr ip, [pc, #4]; add ip, pc, ip; ldr pc, [ip]; .long got - (stub + 12)
STUB_CODE = struct.pack("<III", 0xE59FC004, 0xE08FC00C, 0xE59CF000)
STUB_SIZE = 16


class LinkError(Exception):
    pass


def uleb(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def sleb(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        done = (v == 0 and not b & 0x40) or (v == -1 and b & 0x40)
        out.append(b | (0 if done else 0x80))
        if done:
            return bytes(out)


def align(v, a):
    return (v + a - 1) & ~(a - 1)


def cstr(b, off):
    return b[off:b.index(b"\0", off)].decode()


class Section:
    def __init__(self, raw, data):
        (sect, seg, self.addr, self.size, self.offset, self.align, self.reloff, self.nreloc, self.flags,
         _r1, _r2) = struct.unpack("<16s16sIIIIIIIII", raw)
        self.sectname = sect.rstrip(b"\0").decode()
        self.segname = seg.rstrip(b"\0").decode()
        self.reserved1 = _r1
        self.zerofill = (self.flags & S_TYPE) in (S_ZEROFILL, S_GB_ZEROFILL, S_THREAD_LOCAL_ZEROFILL)
        self.data = bytearray(self.size if self.zerofill else data[self.offset:self.offset + self.size])
        self.relocs = []
        self.new_addr = None

    @property
    def name(self):
        return f"{self.segname},{self.sectname}"


class Reloc:
    __slots__ = ("address", "symbolnum", "pcrel", "length", "extern", "type", "scattered", "value")


def parse_relocs(data, off, n):
    out = []
    for i in range(n):
        w0, w1 = struct.unpack_from("<II", data, off + 8 * i)
        r = Reloc()
        if w0 & 0x80000000:
            r.scattered = True
            r.address = w0 & 0xFFFFFF
            r.type = (w0 >> 24) & 0xF
            r.length = (w0 >> 28) & 3
            r.pcrel = (w0 >> 30) & 1
            r.value = w1
            r.symbolnum = r.extern = None
        else:
            r.scattered = False
            r.address = w0
            r.symbolnum = w1 & 0xFFFFFF
            r.pcrel = (w1 >> 24) & 1
            r.length = (w1 >> 25) & 3
            r.extern = (w1 >> 27) & 1
            r.type = (w1 >> 28) & 0xF
            r.value = None
        out.append(r)
    return out


class Object:
    """An MH_OBJECT (or, for --dump, any 32-bit ARM Mach-O): its sections, symbols, indirect table."""

    def __init__(self, data):
        self.data = data
        magic, cpu, sub, self.filetype, ncmds, _size, self.flags = struct.unpack_from("<7I", data, 0)
        if magic != MH_MAGIC or cpu != CPU_TYPE_ARM:
            raise LinkError("not a 32-bit ARM Mach-O")
        self.sections, self.syms, self.indirect = [], [], []
        self.cmds = []
        off = 28
        symtab = dysymtab = None
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", data, off)
            self.cmds.append((cmd, off, size))
            if cmd == LC_SEGMENT:
                nsects = struct.unpack_from("<I", data, off + 48)[0]
                for i in range(nsects):
                    s = Section(data[off + 56 + 68 * i: off + 56 + 68 * (i + 1)], data)
                    self.sections.append(s)
            elif cmd == LC_SYMTAB:
                symtab = struct.unpack_from("<4I", data, off + 8)
            elif cmd == LC_DYSYMTAB:
                dysymtab = struct.unpack_from("<18I", data, off + 8)
            off += size
        if symtab:
            symoff, nsyms, stroff, _strsize = symtab
            for i in range(nsyms):
                strx, ntype, nsect, ndesc, value = struct.unpack_from("<IBBhI", data, symoff + 12 * i)
                self.syms.append({"name": cstr(data, stroff + strx) if strx else "", "type": ntype,
                                  "sect": nsect, "desc": ndesc, "value": value})
        if dysymtab:
            indirectsymoff, nindirect = dysymtab[12], dysymtab[13]
            self.indirect = list(struct.unpack_from(f"<{nindirect}I", data, indirectsymoff))
        if self.filetype == MH_OBJECT:
            for s in self.sections:
                s.relocs = parse_relocs(data, s.reloff, s.nreloc)

    def section_at(self, addr, end_ok=False):
        """The section containing original address `addr`, or else (when end_ok) one ending at it. Containment
        wins: sections are contiguous, so the address a section ends at is often where the next begins."""
        for s in self.sections:
            if s.addr <= addr < s.addr + s.size:
                return s
        for s in self.sections:
            if end_ok and addr == s.addr + s.size:
                return s
        raise LinkError(f"address 0x{addr:x} is in no section")


def dump(path):
    data = open(path, "rb").read()
    o = Object(data)
    kinds = {}
    print(f"{path}: filetype {o.filetype}, {len(o.sections)} sections, {len(o.syms)} symbols")
    for s in o.sections:
        tally = {}
        for r in s.relocs:
            k = RELOC_NAMES.get(r.type, r.type)
            if not r.scattered and r.type in (ARM_RELOC_VANILLA, ARM_RELOC_BR24):
                k += "/ext" if r.extern else "/local"
            tally[k] = tally.get(k, 0) + 1
            kinds[k] = kinds.get(k, 0) + 1
        print(f"  {s.name:32} addr 0x{s.addr:06x} size 0x{s.size:06x} align {1 << s.align:4} "
              f"flags 0x{s.flags:08x} {tally if tally else ''}")
    undef = sorted(x["name"] for x in o.syms if (x["type"] & N_TYPE) == N_UNDF and x["type"] & N_EXT)
    print(f"  relocation kinds: {kinds}")
    print(f"  {len(undef)} undefined: {' '.join(undef)}")


def export_trie(entries):
    """entries: [(name, offset from the header)] -> the dyld export trie bytes."""
    root = {"children": {}, "terminal": None}
    for name, addr in entries:
        node = root
        for ch in name.encode():
            node = node["children"].setdefault(ch, {"children": {}, "terminal": None})
        node["terminal"] = addr

    # Compress single-child non-terminal chains into edges.
    def build(node):
        edges = []
        for ch, child in sorted(node["children"].items()):
            label = bytes([ch])
            while child["terminal"] is None and len(child["children"]) == 1:
                (c2, nxt), = child["children"].items()
                label += bytes([c2])
                child = nxt
            edges.append((label, build(child)))
        return {"terminal": node["terminal"], "edges": edges, "offset": 0}

    tree = build(root)
    nodes = []

    def order(n):
        nodes.append(n)
        for _, c in n["edges"]:
            order(c)
    order(tree)

    def encode(n):
        out = bytearray()
        if n["terminal"] is not None:
            info = uleb(0) + uleb(n["terminal"])      # flags 0 (regular), address
            out += uleb(len(info)) + info
        else:
            out += b"\0"
        out.append(len(n["edges"]))
        for label, c in n["edges"]:
            out += label + b"\0" + uleb(c["offset"])
        return out

    while True:
        off, changed = 0, False
        for n in nodes:
            if n["offset"] != off:
                n["offset"], changed = off, True
            off += len(encode(n))
        if not changed:
            break
    return b"".join(encode(n) for n in nodes) + b"\0" * (-off % 4)


def link(obj_bytes, install_name, exports, dylibs, current_version=0x10000, compat_version=0x10000,
         min_ios=(5, 0)):
    o = Object(obj_bytes)
    if o.filetype != MH_OBJECT:
        raise LinkError("input is not an MH_OBJECT")
    for s in o.sections:
        if s.segname not in ("__TEXT", "__DATA"):
            raise LinkError(f"section {s.name}: only __TEXT and __DATA are laid out")
        if (s.flags & S_TYPE) in (S_LAZY_SYMBOL_POINTERS, S_SYMBOL_STUBS):
            raise LinkError(f"section {s.name}: lazy pointers / stubs in an object are not handled")

    syms = o.syms
    defined = {x["name"]: x for x in syms if (x["type"] & N_TYPE) == N_SECT and not x["type"] & N_STAB}
    missing = [n for n in exports if n not in defined or not defined[n]["type"] & N_EXT]
    if missing:
        raise LinkError(f"{len(missing)} exports not defined as external symbols: {' '.join(missing[:12])}")

    # Stubs: one per external function a branch reaches.
    stub_targets = []
    for s in o.sections:
        for r in s.relocs:
            if not r.scattered and r.type == ARM_RELOC_BR24 and r.extern:
                name = syms[r.symbolnum]["name"]
                if (syms[r.symbolnum]["type"] & N_TYPE) == N_UNDF and name not in stub_targets:
                    stub_targets.append(name)

    # ---- layout
    load_dylibs = [install_name] + list(dylibs)
    def dylib_cmd_size(name):
        return align(24 + len(name) + 1, 4)
    text_secs = [s for s in o.sections if s.segname == "__TEXT"]
    data_secs = [s for s in o.sections if s.segname == "__DATA" and not s.zerofill] + \
                [s for s in o.sections if s.segname == "__DATA" and s.zerofill]
    nsect_text = len(text_secs) + (1 if stub_targets else 0)
    nsect_data = len(data_secs) + (1 if stub_targets else 0)
    cmds_size = (56 + 68 * nsect_text) + (56 + 68 * nsect_data) + 56 + 48 + \
        sum(dylib_cmd_size(n) for n in load_dylibs) + 24 + 80 + 24 + 16
    addr = align(28 + cmds_size, 16)
    for s in text_secs:
        addr = align(addr, 1 << s.align)
        s.new_addr = addr
        addr += s.size
    stubs_addr = align(addr, 4)
    addr = stubs_addr + STUB_SIZE * len(stub_targets)
    text_size = align(addr, PAGE)
    data_vm = text_size
    addr = data_vm
    for s in data_secs:
        addr = align(addr, 1 << s.align)
        s.new_addr = addr
        addr += s.size
    data_file_end = max([s.new_addr + s.size for s in data_secs if not s.zerofill] + [data_vm])
    got_addr = None
    if stub_targets:
        # the GOT goes before the zero-fill sections, so it has file bytes
        zf = [s for s in data_secs if s.zerofill]
        got_addr = align(data_file_end, 4)
        if zf:
            first = zf[0].new_addr
            new = align(got_addr + 4 * len(stub_targets), 1 << zf[0].align)
            for s in zf:
                s.new_addr += new - first
        data_file_end = got_addr + 4 * len(stub_targets)
        addr = max([s.new_addr + s.size for s in data_secs] + [data_file_end])
    data_vmsize = align(addr - data_vm, PAGE)
    data_filesize = align(data_file_end - data_vm, PAGE)

    image = bytearray(text_size + data_filesize)
    for s in text_secs + data_secs:
        if not s.zerofill:
            image[s.new_addr:s.new_addr + s.size] = s.data

    def new_of(orig, end_ok=False):
        s = o.section_at(orig, end_ok)
        return s.new_addr + (orig - s.addr)

    def segment_of(addr):
        if addr < text_size:
            return 0, addr
        return 1, addr - data_vm

    rebases, binds = [], []          # (seg, offset), (seg, offset, name, addend)

    def put32(a, v):
        struct.pack_into("<I", image, a, v & 0xFFFFFFFF)

    def get32(a):
        return struct.unpack_from("<I", image, a)[0]

    # Stubs and their GOT slots.
    stub_of = {}
    for i, name in enumerate(stub_targets):
        sa, ga = stubs_addr + STUB_SIZE * i, got_addr + 4 * i
        image[sa:sa + 12] = STUB_CODE
        put32(sa + 12, ga - (sa + 12))
        stub_of[name] = sa
        binds.append((1, ga - data_vm, name, 0))

    # Relocations.
    for s in o.sections:
        rel = iter(s.relocs)
        for r in rel:
            at = s.new_addr + r.address
            if r.length != 2:
                raise LinkError(f"{s.name}+0x{r.address:x}: {RELOC_NAMES.get(r.type)} of length {r.length}")
            if r.scattered:
                if r.type in (ARM_RELOC_SECTDIFF, ARM_RELOC_LOCAL_SECTDIFF):
                    pair = next(rel)
                    if pair.type != ARM_RELOC_PAIR:
                        raise LinkError(f"{s.name}+0x{r.address:x}: SECTDIFF without a PAIR")
                    a_orig, b_orig = r.value, pair.value
                    content = struct.unpack_from("<i", s.data, r.address)[0]
                    target = content + b_orig            # A + offset, original addresses
                    new_a = new_of(a_orig) + (target - a_orig)
                    put32(at, new_a - new_of(b_orig, end_ok=True))
                    continue
                if r.type == ARM_RELOC_VANILLA:      # scattered vanilla: a pointer to r.value + offset
                    content = struct.unpack_from("<I", s.data, r.address)[0]
                    put32(at, new_of(r.value) + (content - r.value))
                    if s.segname != "__DATA":
                        raise LinkError(f"{s.name}+0x{r.address:x}: a text relocation")
                    rebases.append(segment_of(at))
                    continue
                raise LinkError(f"{s.name}+0x{r.address:x}: scattered {RELOC_NAMES.get(r.type, r.type)}")
            if r.type == ARM_RELOC_BR24:
                insn = struct.unpack_from("<I", s.data, r.address)[0]
                pc_orig = s.addr + r.address
                if r.extern:
                    sym = syms[r.symbolnum]
                    if (sym["type"] & N_TYPE) == N_UNDF:
                        target_new = stub_of[sym["name"]]
                    else:
                        target_new = new_of(sym["value"])
                else:
                    disp = ((insn & 0xFFFFFF) ^ 0x800000) - 0x800000
                    target_new = new_of(pc_orig + 8 + (disp << 2))
                if insn >> 28 == 0xF:                # blx imm -> bl: everything we reach is ARM
                    insn = 0xEB000000
                disp = target_new - (at + 8)
                if disp & 3 or not -(1 << 25) <= disp < (1 << 25):
                    raise LinkError(f"{s.name}+0x{r.address:x}: branch out of range")
                put32(at, (insn & 0xFF000000) | ((disp >> 2) & 0xFFFFFF))
                continue
            if r.type == ARM_RELOC_VANILLA:
                content = struct.unpack_from("<I", s.data, r.address)[0]
                if s.segname != "__DATA":
                    raise LinkError(f"{s.name}+0x{r.address:x}: a text relocation (VANILLA in __TEXT)")
                if r.extern:
                    sym = syms[r.symbolnum]
                    if (sym["type"] & N_TYPE) == N_UNDF:
                        put32(at, 0)
                        binds.append(segment_of(at) + (sym["name"], content))
                    else:
                        put32(at, new_of(sym["value"]) + content)
                        rebases.append(segment_of(at))
                else:
                    put32(at, new_of(content, end_ok=True))
                    rebases.append(segment_of(at))
                continue
            raise LinkError(f"{s.name}+0x{r.address:x}: {RELOC_NAMES.get(r.type, r.type)} is not handled")

    # The object's own non-lazy pointers (code reaching external or far data): bind or rebase each entry.
    for s in o.sections:
        if (s.flags & S_TYPE) != S_NON_LAZY_SYMBOL_POINTERS:
            continue
        for i in range(s.size // 4):
            idx = o.indirect[s.reserved1 + i]
            at = s.new_addr + 4 * i
            if idx & INDIRECT_SYMBOL_ABS:
                continue
            if idx & INDIRECT_SYMBOL_LOCAL:
                put32(at, new_of(struct.unpack_from("<I", s.data, 4 * i)[0]))
                rebases.append(segment_of(at))
                continue
            sym = syms[idx]
            if (sym["type"] & N_TYPE) == N_UNDF:
                put32(at, 0)
                binds.append(segment_of(at) + (sym["name"], 0))
            else:
                put32(at, new_of(sym["value"]))
                rebases.append(segment_of(at))

    # ---- __LINKEDIT
    rebase_ops = bytearray([REBASE_OPCODE_SET_TYPE_IMM | REBASE_TYPE_POINTER])
    for seg, off in sorted(set(rebases)):
        rebase_ops += bytes([REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | seg]) + uleb(off)
        rebase_ops += bytes([REBASE_OPCODE_DO_REBASE_IMM_TIMES | 1])
    rebase_ops += bytes([REBASE_OPCODE_DONE])
    rebase_ops += b"\0" * (-len(rebase_ops) % 4)

    bind_ops = bytearray([BIND_OPCODE_SET_DYLIB_SPECIAL_IMM | (BIND_SPECIAL_DYLIB_FLAT_LOOKUP & 0xF),
                          BIND_OPCODE_SET_TYPE_IMM | BIND_TYPE_POINTER])
    for seg, off, name, addend in sorted(binds, key=lambda b: (b[2], b[0], b[1])):
        bind_ops += bytes([BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM]) + name.encode() + b"\0"
        bind_ops += bytes([BIND_OPCODE_SET_ADDEND_SLEB]) + sleb(addend)
        bind_ops += bytes([BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | seg]) + uleb(off)
        bind_ops += bytes([BIND_OPCODE_DO_BIND])
    bind_ops += bytes([BIND_OPCODE_DONE])
    bind_ops += b"\0" * (-len(bind_ops) % 4)

    export_addrs = sorted((n, new_of(defined[n]["value"])) for n in exports)
    trie = export_trie(export_addrs)

    imports = sorted({b[2] for b in binds})
    strtab = bytearray(b" \0")
    nlist = bytearray()
    sect_index = {}
    for i, s in enumerate(text_secs + ([None] if stub_targets else []) + data_secs, 1):
        if s is not None:
            sect_index[id(s)] = i
    for n, a in export_addrs:
        s = o.section_at(defined[n]["value"])
        nlist += struct.pack("<IBBHI", len(strtab), N_SECT | N_EXT, sect_index[id(s)], 0, a)
        strtab += n.encode() + b"\0"
    for n in imports:
        # n_desc's library ordinal: DYNAMIC_LOOKUP_ORDINAL (0xfe), the symbol table's word for a flat lookup
        nlist += struct.pack("<IBBHI", len(strtab), N_UNDF | N_EXT, 0, 0xFE00, 0)
        strtab += n.encode() + b"\0"
    strtab += b"\0" * (-len(strtab) % 4)

    linkedit_off = len(image)
    rebase_off = linkedit_off
    bind_off = rebase_off + len(rebase_ops)
    export_off = bind_off + len(bind_ops)
    sym_off = export_off + len(trie)
    str_off = sym_off + len(nlist)
    linkedit = bytes(rebase_ops) + bytes(bind_ops) + trie + bytes(nlist) + bytes(strtab)
    linkedit_size = len(linkedit)

    # ---- load commands
    def section_hdr(name, seg, a, size, fileoff, al, flags):
        return struct.pack("<16s16sIIIIIIIII", name.encode(), seg.encode(), a, size, fileoff, al, 0, 0, flags, 0, 0)

    def segment(name, vm, vmsize, fileoff, filesize, prot, sects):
        return struct.pack("<II16sIIIIIIII", LC_SEGMENT, 56 + 68 * len(sects), name.encode(), vm, vmsize,
                           fileoff, filesize, prot, prot, len(sects), 0) + b"".join(sects)

    tsects = [section_hdr(s.sectname, "__TEXT", s.new_addr, s.size, s.new_addr, s.align, s.flags)
              for s in text_secs]
    if stub_targets:
        tsects.append(section_hdr("__stubs", "__TEXT", stubs_addr, STUB_SIZE * len(stub_targets), stubs_addr, 2,
                                  S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS))
    dsects = []
    for s in data_secs:
        flags = s.flags
        if (flags & S_TYPE) == S_NON_LAZY_SYMBOL_POINTERS:
            flags &= ~S_TYPE            # bound by opcodes; no indirect symbol table to go with it
        dsects.append(section_hdr(s.sectname, "__DATA", s.new_addr, s.size, 0 if s.zerofill else s.new_addr,
                                  s.align, flags))
    if stub_targets:
        dsects.insert(len([s for s in data_secs if not s.zerofill]),
                      section_hdr("__got", "__DATA", got_addr, 4 * len(stub_targets), got_addr, 2, 0))

    cmds = segment("__TEXT", 0, text_size, 0, text_size, 5, tsects)
    cmds += segment("__DATA", data_vm, data_vmsize, text_size, data_filesize, 3, dsects)
    cmds += segment("__LINKEDIT", data_vm + data_vmsize, align(linkedit_size, PAGE), linkedit_off,
                    linkedit_size, 1, [])
    cmds += struct.pack("<12I", LC_DYLD_INFO_ONLY, 48, rebase_off, len(rebase_ops), bind_off, len(bind_ops),
                        0, 0, 0, 0, export_off, len(trie))
    for i, n in enumerate(load_dylibs):
        size = dylib_cmd_size(n)
        cmd = LC_ID_DYLIB if i == 0 else LC_LOAD_DYLIB
        ver = (current_version, compat_version) if i == 0 else (0x10000, 0x10000)
        cmds += (struct.pack("<IIIIII", cmd, size, 24, 2, *ver) + n.encode()).ljust(size, b"\0")
    cmds += struct.pack("<6I", LC_SYMTAB, 24, sym_off, len(nlist) // 12, str_off, len(strtab))
    nexp = len(export_addrs)
    cmds += struct.pack("<20I", LC_DYSYMTAB, 80, 0, 0, 0, nexp, nexp, len(imports), *([0] * 12))
    uuid = hashlib.md5(bytes(image) + linkedit).digest()
    cmds += struct.pack("<II", LC_UUID, 24) + uuid
    cmds += struct.pack("<IIII", LC_VERSION_MIN_IPHONEOS, 16, (min_ios[0] << 16) | (min_ios[1] << 8),
                        (min_ios[0] << 16) | (min_ios[1] << 8))
    if len(cmds) != cmds_size:
        raise LinkError(f"load command size {len(cmds)} != planned {cmds_size}")
    header = struct.pack("<7I", MH_MAGIC, CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, MH_DYLIB, 4 + len(load_dylibs) + 4,
                         len(cmds), MH_DYLDLINK | MH_TWOLEVEL | MH_NO_REEXPORTED_DYLIBS)
    if len(header) + len(cmds) > text_secs[0].new_addr:
        raise LinkError("load commands overrun the first section")
    image[0:len(header) + len(cmds)] = header + cmds
    out = bytes(image) + linkedit
    info = {"text": text_size, "data": data_vmsize, "stubs": len(stub_targets), "rebases": len(set(rebases)),
            "binds": len(binds), "exports": nexp, "imports": imports, "bytes": len(out)}
    return out, info


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("obj", nargs="?")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--dump")
    ap.add_argument("--install-name")
    ap.add_argument("--exports", help="file with one symbol per line (ld's -exported_symbols_list)")
    ap.add_argument("--dylib", action="append", default=[])
    a = ap.parse_args()
    if a.dump:
        dump(a.dump)
        return
    exports = [l.strip() for l in open(a.exports) if l.strip() and not l.startswith("#")]
    out, info = link(open(a.obj, "rb").read(), a.install_name, exports, a.dylib)
    open(a.out, "wb").write(out)
    print({k: v for k, v in info.items() if k != "imports"})
    print("imports:", " ".join(info["imports"]))


if __name__ == "__main__":
    try:
        main()
    except LinkError as e:
        sys.exit(f"macho_dylib: {e}")
