"""lockdownd's activation short-circuit, taken unconditionally (6.1.x, e.g. 10B329).

6.1's lockdownd (`dealwith_activation`) has a development shortcut: unless the data ark sets
DisableHactivation, it asks a predicate and, when that answers 1, logs "Short circuiting activation state
to Activated." and writes ActivationState = Activated into the ark itself. Otherwise it looks for an
Apple-issued activation record, which an emulated unit cannot have, so a seeded FactoryActivated does not
hold past it (docs/n81/README.md: the data ark was enough up to 10A523, not from 10B144 on). Two edits make
the shortcut unconditional:

1. the predicate's `bl` becomes `movs r0, #1; movs r0, #1` (its `cmp r0, #1; beq.w` then always branches
   to the block that loads the "Short circuiting ..." CFString);
2. the `bne` that, for a DisableHactivation value other than kCFBooleanTrue, releases it and falls into the
   predicate becomes a `b` straight to the predicate, so no ark value can turn the shortcut off.

Both are found by shape and by the CFStrings the blocks log, never by offset, and each must match exactly
once or nothing is written (fail closed). The code signature's page hashes are recomputed for the pages
touched (the kernel runs with AMFI off here, so this only keeps the signature self-consistent).
Cross-checked: on 10B329 the result is byte-identical to Legacy-iOS-Kit's hacktivation bspatch for that
build (its Down_iPhone2,1_6.1.3_10B329.bundle/lockdownd.patch).

    python lockdownd_patch.py LOCKDOWND OUT
"""
import hashlib
import struct
import sys

from macho import MachO

SHORTCUT = b"Short circuiting activation state to Activated."
DISABLED = b"Disabling activation short-circuit because DisableHactivation is set"
MOVS_R0_1 = b"\x01\x20"
LC_CODE_SIGNATURE = 0x1d


class PatchError(Exception):
    pass


def hw(d, o):
    return struct.unpack_from("<H", d, o)[0]


def is32(h):
    return (h >> 11) in (0x1d, 0x1e, 0x1f)


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def bl_target(d, o, vm):
    """The target of a Thumb BL (T1) at file offset o / address vm, or None."""
    h1, h2 = hw(d, o), hw(d, o + 2)
    if (h1 & 0xF800) != 0xF000 or (h2 & 0xD000) != 0xD000:
        return None
    s = (h1 >> 10) & 1
    j1, j2 = (h2 >> 13) & 1, (h2 >> 11) & 1
    i1, i2 = 1 - (j1 ^ s), 1 - (j2 ^ s)
    off = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
    return vm + 4 + sext(off, 25)


def beqw_target(d, o, vm):
    """The target of a Thumb B<EQ>.W (T3) at o / vm, or None."""
    h1, h2 = hw(d, o), hw(d, o + 2)
    if (h1 & 0xF800) != 0xF000 or (h2 & 0xD000) != 0x8000 or ((h1 >> 6) & 0xF) != 0:
        return None
    s = (h1 >> 10) & 1
    j1, j2 = (h2 >> 13) & 1, (h2 >> 11) & 1
    off = (s << 20) | (j2 << 19) | (j1 << 18) | ((h1 & 0x3F) << 12) | ((h2 & 0x7FF) << 1)
    return vm + 4 + sext(off, 21)


def pc_loads(d, o, vm, length):
    """Addresses formed by movw/movt + `add rd, pc` pairs in the instructions of [vm, vm + length)."""
    lo, hi, out = {}, {}, set()
    end = o + length
    while o < end:
        h1 = hw(d, o)
        if is32(h1):
            h2 = hw(d, o + 2)
            if (h1 & 0xFB70) == 0xF240:                       # movw / movt (T3 / T1)
                imm = ((h1 & 0xF) << 12) | (((h1 >> 10) & 1) << 11) | (((h2 >> 12) & 7) << 8) | (h2 & 0xFF)
                rd = (h2 >> 8) & 0xF
                (hi if h1 & 0x80 else lo)[rd] = imm
            o, vm = o + 4, vm + 4
            continue
        if (h1 & 0xFF78) == 0x4478:                            # add rd, pc
            rd = (h1 & 7) | ((h1 >> 4) & 8)
            if rd in lo:
                out.add(((hi.get(rd, 0) << 16 | lo[rd]) + vm + 4) & 0xFFFFFFFF)
        o, vm = o + 2, vm + 2
    return out


class Image:
    def __init__(self, data):
        self.d = bytearray(data)
        self.m = MachO(bytes(data))
        if self.m.cputype != 12:
            raise PatchError("not an ARM Mach-O")
        self.text = self.section("__TEXT", "__text")
        self.text_seg = self.m.segment("__TEXT")

    def section(self, seg, name):
        s = self.m.segment(seg)
        for sname, addr, size, off in (s.sections if s else []):
            if sname == name:
                return addr, size, off
        raise PatchError(f"no {seg},{name}")

    def off(self, vm):
        for s in self.m.segments:
            if s.vmaddr <= vm < s.vmaddr + s.filesize:
                return s.fileoff + vm - s.vmaddr
        raise PatchError(f"address 0x{vm:x} is not in the file")

    def cfstring(self, text):
        """The address of the one __cfstring entry whose characters are `text`."""
        addr, size, off = self.section("__TEXT", "__cstring")
        hits = []
        at = self.d.find(text + b"\0", off, off + size)
        while at >= 0:
            if at == off or self.d[at - 1] == 0:
                hits.append(addr + at - off)
            at = self.d.find(text + b"\0", at + 1, off + size)
        if len(hits) != 1:
            raise PatchError(f"{text.decode()!r}: {len(hits)} C strings, expected 1")
        caddr, csize, coff = self.section("__DATA", "__cfstring")
        cfs = [caddr + i for i in range(0, csize, 16)
               if struct.unpack_from("<I", self.d, coff + i + 8)[0] == hits[0]]
        if len(cfs) != 1:
            raise PatchError(f"{text.decode()!r}: {len(cfs)} CFStrings, expected 1")
        return cfs[0]


def find_sites(img):
    """(predicate bl, guard bne) file offsets, each the only match; or `already` when patched before."""
    shortcut, disabled = img.cfstring(SHORTCUT), img.cfstring(DISABLED)
    addr, size, off = img.text
    preds, done = [], []
    for k in range(0, size - 10, 2):
        o, vm = off + k, addr + k
        if hw(img.d, o + 4) != 0x2801:                         # cmp r0, #1
            continue
        target = beqw_target(img.d, o + 6, vm + 6)
        if target is None or shortcut not in pc_loads(img.d, img.off(target), target, 32):
            continue
        if bytes(img.d[o:o + 4]) == MOVS_R0_1 * 2:
            done.append(o)
        elif bl_target(img.d, o, vm) is not None:
            preds.append(o)
    if done and not preds:
        return None, None, done
    if len(preds) != 1:
        raise PatchError(f"expected one shortcut predicate (bl; cmp r0,#1; beq.w -> {SHORTCUT.decode()!r}), "
                         f"found {len(preds)}")
    bl = preds[0]
    bl_vm = addr + bl - off
    # the guard: a bne.n in the 64 bytes before the bl that lands 6 bytes before it (mov r0, rX; blx CFRelease),
    # whose fall-through logs the DisableHactivation message
    guards = []
    for o in range(max(off, bl - 64), bl, 2):
        h = hw(img.d, o)
        if (h & 0xFF00) != 0xD100:
            continue
        vm = addr + o - off
        if vm + 4 + (sext(h & 0xFF, 8) << 1) != bl_vm - 6:
            continue
        if disabled in pc_loads(img.d, o + 2, vm + 2, bl - 6 - (o + 2)):
            guards.append(o)
    if len(guards) != 1:
        raise PatchError(f"expected one DisableHactivation guard before the predicate, found {len(guards)}")
    return bl, guards[0], []


def fix_page_hashes(img, offsets):
    """Recompute the CodeDirectory's code-slot hashes for the pages holding `offsets`."""
    d, at = img.d, 28
    for _ in range(img.m.ncmds):
        cmd, size = struct.unpack_from("<II", d, at)
        if cmd == LC_CODE_SIGNATURE:
            break
        at += size
    else:
        return 0
    sig, _ = struct.unpack_from("<II", d, at + 8)
    magic, _, count = struct.unpack_from(">III", d, sig)
    if magic != 0xFADE0CC0:
        raise PatchError("code signature: not a SuperBlob")
    fixed = 0
    for i in range(count):
        _, boff = struct.unpack_from(">II", d, sig + 12 + 8 * i)
        cd = sig + boff
        if struct.unpack_from(">I", d, cd)[0] != 0xFADE0C02:
            continue
        hash_off, _, _, ncode, limit = struct.unpack_from(">IIIII", d, cd + 16)
        hsize, htype, _, page_log = struct.unpack_from(">BBBB", d, cd + 36)
        if (hsize, htype) != (20, 1):
            raise PatchError(f"code signature: hash type {htype} size {hsize}, expected SHA-1")
        ps = 1 << page_log
        for page in sorted({o // ps for o in offsets}):
            if page >= ncode:
                raise PatchError("code signature: patched page outside the code slots")
            slot = cd + hash_off + 20 * page
            d[slot:slot + 20] = hashlib.sha1(bytes(d[page * ps:min((page + 1) * ps, limit)])).digest()
            fixed += 1
    return fixed


def patch(data):
    """(patched bytes, report). Raises PatchError, touching nothing, when the binary does not match."""
    img = Image(data)
    bl, bne, done = find_sites(img)
    if done:
        return bytes(data), "already patched (the shortcut's predicate is movs r0, #1)"
    addr, _, off = img.text
    bl_vm, bne_vm = addr + bl - off, addr + bne - off
    img.d[bl:bl + 4] = MOVS_R0_1 * 2
    img.d[bne:bne + 2] = struct.pack("<H", 0xE000 | (((bl_vm - (bne_vm + 4)) >> 1) & 0x7FF))
    pages = fix_page_hashes(img, (bl, bne))
    return bytes(img.d), (f"predicate bl at 0x{bl_vm:x} -> movs r0,#1 x2; DisableHactivation bne at 0x{bne_vm:x} "
                          f"-> b 0x{bl_vm:x}; {pages} code-signature page hash(es) updated")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip().splitlines()[-1].strip())
    data = open(sys.argv[1], "rb").read()
    try:
        new, report = patch(data)
    except (PatchError, ValueError) as e:
        raise SystemExit(f"lockdownd_patch: {e}; nothing written")
    open(sys.argv[2], "wb").write(new)
    print(f"lockdownd_patch: {report}; sha256 {hashlib.sha256(new).hexdigest()}")


if __name__ == "__main__":
    main()
