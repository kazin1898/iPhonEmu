"""A 32-bit Mach-O's segments, entry point and the boot_args.Version its pe_identify_machine demands."""
import re
import struct
from dataclasses import dataclass


@dataclass
class Segment:
    name: str
    vmaddr: int
    vmsize: int
    fileoff: int
    filesize: int
    sections: list


class MachO:
    def __init__(self, data):
        self.data = data
        if len(data) < 28 or struct.unpack_from("<I", data, 0)[0] != 0xFEEDFACE:
            raise ValueError("not a 32-bit Mach-O")
        (self.cputype, self.cpusubtype, self.filetype, self.ncmds) = struct.unpack_from("<4I", data, 4)
        self.segments = []
        self._entry = None
        off = 28
        for _ in range(self.ncmds):
            cmd, size = struct.unpack_from("<II", data, off)
            if cmd == 1:
                name = data[off + 8:off + 24].split(b"\0")[0].decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<4I", data, off + 24)
                nsects = struct.unpack_from("<I", data, off + 48)[0]
                sects = []
                so = off + 56
                for _ in range(nsects):
                    sname = data[so:so + 16].split(b"\0")[0].decode()
                    addr, ssize, soff = struct.unpack_from("<3I", data, so + 32)
                    sects.append((sname, addr, ssize, soff))
                    so += 68
                self.segments.append(Segment(name, vmaddr, vmsize, fileoff, filesize, sects))
            elif cmd == 5:
                self._entry = struct.unpack_from("<I", data, off + 16 + 15 * 4)[0]
            if size == 0:
                break
            off += size

    def segment(self, name):
        for s in self.segments:
            if s.name == name:
                return s
        return None

    def entry(self):
        if self._entry is None:
            raise ValueError("no LC_UNIXTHREAD")
        return self._entry

    def boot_args_version(self):
        """2, or the immediate of the `ldrh rN, [r0, #2]; cmp rN, #V` pair that guards
        "pe_identify_machine: Epoch Mismatch" (3 from xnu-1735.47). iOS 6's xnu-2107 reaches the
        string with movw/movt/add pc, which the second scan recognises (FirmwareKit KBoot.swift)."""
        d = self.data
        so = d.find(b"pe_identify_machine: Epoch Mismatch")
        if so < 0:
            return 2
        seg = next((s for s in self.segments if s.fileoff <= so < s.fileoff + s.filesize), None)
        if seg is None:
            return 2
        sva = (seg.vmaddr + so - seg.fileoff) & 0xFFFFFFFF
        lit = d.find(struct.pack("<I", sva))
        if lit >= 0:
            window = d[max(0, lit - 0x400):lit]
            for n in range(8):
                idx = [i for i in range(len(window) - 1)
                       if window[i] == (0x40 | n) and window[i + 1] == 0x88]
                if not idx:
                    continue
                i = idx[-1]
                for j in range(i + 2, min(i + 10, len(window))):
                    if window[j] == (0x28 | n):
                        return window[j - 1]

        def h16(o):
            return d[o] | d[o + 1] << 8

        def imm16(hw1, hw2):
            return (hw1 & 0xF) << 12 | ((hw1 >> 10) & 1) << 11 | ((hw2 >> 12) & 7) << 8 | (hw2 & 0xFF)

        def names_string(at, seg):
            for o in range(at, min(at + 24, len(d) - 10), 2):
                hw1, hw2 = h16(o), h16(o + 2)
                if hw1 & 0xFBF0 != 0xF240:
                    continue
                rd = (hw2 >> 8) & 0xF
                lo = imm16(hw1, hw2)
                t1, t2 = h16(o + 4), h16(o + 6)
                if t1 & 0xFBF0 != 0xF2C0 or (t2 >> 8) & 0xF != rd:
                    continue
                if h16(o + 8) != (0x4478 | (rd & 7) | ((rd & 8) << 4)):
                    continue
                pc = (seg.vmaddr + (o + 8 - seg.fileoff) + 4) & 0xFFFFFFFF
                return ((imm16(t1, t2) << 16 | lo) + pc) & 0xFFFFFFFF == sva
            return False

        for seg in self.segments:
            if seg.filesize == 0:
                continue
            lo, hi = seg.fileoff, min(seg.fileoff + seg.filesize, len(d) - 4)
            for i in range(lo, hi, 2):
                if d[i + 1] == 0x88 and d[i] & 0xF8 == 0x40:
                    n = d[i] & 7
                    if d[i + 3] == (0x28 | n) and names_string(i + 6, seg):
                        return d[i + 2]
        return 2

    def prelink_bundle_ids(self):
        """CFBundleIdentifiers of the prelinked kexts (__PRELINK_INFO)."""
        seg = self.segment("__PRELINK_INFO")
        if not seg:
            return []
        info = self.data[seg.fileoff:seg.fileoff + seg.filesize]
        ids = re.findall(rb"<key>CFBundleIdentifier</key>\s*<string>([^<]+)</string>", info)
        return sorted({i.decode() for i in ids})

    def version_string(self):
        m = re.search(rb"Darwin Kernel Version[^\x00]*", self.data)
        return m.group(0).decode() if m else None
