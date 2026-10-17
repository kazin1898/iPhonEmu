"""The synthetic PPN NAND the iPhone 4S machine serves (hw/arm/s5l8940_iop.c): one geometry, written
both into the kboot device tree (what iBoot would have filled into arm-io/flash-controller0/disk from
the chips) and into the page store's geometry.json, so the kernel and the model agree.

The 10B329 kernel has only the PPN FTL (AppleSwissPPNFTL); IOFlashStorageDevice-664.13.12 takes the
geometry from the disk node (docs/n94/hardware.md "The IOP") and AppleIOPFMI-89.3 publishes the chip ID
it read from the IOP on the nub. The numbers below are a consistent 8 GiB part, not a real chip: two
channels (FMI0, FMI1) with one chip enable each, two CAUs per chip, 2048 blocks per CAU of 128 pages
of 8 KiB (+32 bytes of meta, 16 per 4 KiB logical page), 64 SLC pages per block, 1536-byte boot pages.
Row addresses are (cau << (block_bits + page_bits)) | (block << page_bits) | page.
"""
import ctypes
import json
import os
import struct
import sys

PPN = {
    "format": "ppn-xor-ff-v1",
    "channels": 2,
    "ce_per_channel": 1,
    "caus_per_ce": 2,
    "blocks_per_cau": 2048,
    "pages_per_block": 128,
    "slc_pages_per_block": 64,
    "page_bytes": 8192,
    "meta_bytes": 32,
    "logical_page_bytes": 4096,
    "boot_page_bytes": 0x600,
    "cau_bits": 1,
    "block_bits": 11,
    "page_bits": 7,
    "chip_id": "983a9493765a",         # 6 bytes, as AppleIOPFMI prints them
    "ppn_spec_version": 0x10000,       # PPN 1.0
}


def dt_props(g=PPN):
    """(key, value) for arm-io/flash-controller0 and its disk child; only keys the DT has get written."""
    ces = g["channels"] * g["ce_per_channel"]
    ce_bitmap = 0
    for ch in range(g["channels"]):
        for ce in range(g["ce_per_channel"]):
            ce_bitmap |= 1 << (ch * 8 + ce)
    blocks_per_ce = g["caus_per_ce"] * g["blocks_per_cau"]
    meta_per_lp = g["meta_bytes"] * g["logical_page_bytes"] // g["page_bytes"]
    chip_id = bytes.fromhex(g["chip_id"])
    return [
        ("ppn-device", 1), ("ppn-spec-version", g["ppn_spec_version"]), ("ppn-ftl-ver", 2),
        ("#ce", ces), ("ce-bitmap", ce_bitmap), ("#databus", g["channels"]), ("#die-ce", g["caus_per_ce"]),
        ("#ce-blocks", blocks_per_ce), ("#block-pages", g["pages_per_block"]), ("#page-bytes", g["page_bytes"]),
        ("#spare-bytes", g["meta_bytes"]), ("#page-sectors", g["page_bytes"] // 512),
        ("meta-per-logical-page", meta_per_lp), ("valid-meta-per-logical-page", meta_per_lp),
        ("logical-page-size", g["logical_page_bytes"]), ("#bootloader-bytes", g["boot_page_bytes"]),
        ("caus-ce", g["caus_per_ce"]), ("blocks-cau", g["blocks_per_cau"]),
        ("cau-bits", g["cau_bits"]), ("block-bits", g["block_bits"]), ("page-bits", g["page_bits"]),
        ("block-stride", 1 << g["page_bits"]), ("slc-pages", g["slc_pages_per_block"]),
        ("mlc-pages", g["pages_per_block"]), ("match-oddeven-caus", 0), ("max-transaction-size", 64),
        ("vendor-type", 0), ("bbt-format", 0), ("ecc-threshold", 0), ("ecc-correctable", 0),
        ("toggle-device", 0), ("banks-per-ce", 1),
        ("device-readid", chip_id + b"\0\0"), ("dbg-chipids", (chip_id + b"\0\0") * ces),
    ]


# ---- the partition table in the boot blocks (IOFlashStorage-664.13.12's nand_part_core) -------------
#
# Decoded from IOFlashPartitionScheme's reader/builder (docs/n94/hardware.md "Storage: the partition
# table"): a 1536-byte boot page, written to pages 0 and 1 of block 0 of every chip. 0x000 'ndrG',
# 0x004 0, 0x008 versionMajor 1, 0x00c versionMinor 9, 0x010 generation; 0x020 factory BBT (none);
# 0x200 geometry {0, 0, 1, ceCount, blocksPerCE, pagesPerMLCBlock, sectorsPerPage, bytesPerMeta, flags
# 7 (3 | 4 isPpn)}; 0x224 cemetery {gate 0xdeadcafe, flags, plots[32], guards}; 0x250 spares {'rAps',
# count, flags, locations[64], guards at 0x10a}; 0x35c substitutions {'sbus', count, flags, maps[64],
# guards at 0x4a}; 0x400 32 entries {content, width/offset, depth, type|flags}. The guards are the
# Fletcher-16 of the bytes before them. The entries are what the kernel builds from its PPN format string
# "slice:boot:1 pool:plog:4:1 pool:nvrm:2:3 flex:firm:2:s:/:firmware-min-capacity bulk:fsys info:scfg
# info:diag info:fbbt": a slice takes depth blocks on every chip, a pool width x depth blocks in all, the
# bulk the rest (offset = blocks taken so far, depth = blocksPerCE - offset), an info entry nothing.
ENTRY_SLICE, ENTRY_INFO, ENTRY_BULK, ENTRY_POOL = 2, 1, 4, 8
POOL_SLC, POOL_FULL_PAGES = 0x100, 0x200


def fletcher16(data):
    s1 = s2 = 0xff
    for i in range(0, len(data), 20):
        for b in data[i:i + 20]:
            s1 += b
            s2 += s1
        s1 = (s1 & 0xff) + (s1 >> 8)
        s2 = (s2 & 0xff) + (s2 >> 8)
    s1 = (s1 & 0xff) + (s1 >> 8)
    s2 = (s2 & 0xff) + (s2 >> 8)
    return (s2 << 8 | s1) & 0xffff


def fourcc(name):
    """An entry's content as nand_part compares it: the integer 'boot' (0x626f6f74), stored little-endian."""
    return int.from_bytes(name.encode(), "big")


def partition_specs(g=PPN, firmware_min_capacity=0x180000):
    """(name, type, a, b, flags) as nand_part lays them out for this geometry."""
    ces = g["channels"] * g["ce_per_channel"]
    blocks_per_ce = g["caus_per_ce"] * g["blocks_per_cau"]
    slc_block_bytes = g["slc_pages_per_block"] * g["page_bytes"]
    firm_depth = -(-firmware_min_capacity // slc_block_bytes)
    specs = []
    used = 0
    specs.append(("boot", ENTRY_SLICE, 0, 1, 0))
    used += 1 * ces
    for name, width, depth, flags in (("plog", 4, 1, POOL_SLC), ("nvrm", 2, 3, POOL_SLC),
                                      ("firm", 2, firm_depth, POOL_SLC | POOL_FULL_PAGES)):
        specs.append((name, ENTRY_POOL, width, depth, flags))
        used += width * depth
    specs.append(("fsys", ENTRY_BULK, used, blocks_per_ce - used, 2))
    for name in ("scfg", "diag", "fbbt"):
        specs.append((name, ENTRY_INFO, 0, 1, 0))
    return specs


def pool_map(g, specs):
    """(spares [(ce, block)], subs [vblock]) for the pool partitions, as the kernel's own "reborn"
    rewrite of the table lays them out (read back from a boot block it wrote): pool slot s of width w
    is chip s % chips at per-chip offset offset + s // chips, and its depth blocks j are virtual
    blocks (offset + s // chips + j) * chips + s % chips; spare i is handed out by the VFL from the
    top of its banks (chip, CAU), round robin from the last bank, block top - 4 - (i + 1) // banks;
    the kernel asks for the pool blocks plus 4 extra, mapped to vblock 0."""
    ces = g["channels"] * g["ce_per_channel"]
    banks = ces * g["caus_per_ce"]
    subs = []
    offset = 0
    for name, typ, a, b, flags in specs:
        if typ == ENTRY_SLICE:
            offset += b
        elif typ == ENTRY_POOL:
            for slot in range(a):
                for j in range(b):
                    subs.append((offset + slot // ces + j) * ces + slot % ces)
            offset += a * b // ces
    subs += [0] * 4         # the extra spares the kernel asks for on a PPN table (versionMajor 1)
    spares = []
    for i in range(len(subs)):
        bank = (banks - 1 + i) % banks
        ce, cau = bank % ces, bank // ces
        spares.append((ce, (cau + 1) * g["blocks_per_cau"] - 4 - (i + 1) // banks))
    assert len(spares) <= 64 and all(v < 256 for v in subs)
    return spares, subs


def build_ptab(g=PPN, generation=1, firmware_min_capacity=0x180000):
    page = bytearray(g["boot_page_bytes"])
    page[0:4] = b"ndrG"
    struct.pack_into("<IIII", page, 8, 1, 9, generation, 0)
    ces = g["channels"] * g["ce_per_channel"]
    struct.pack_into("<IIIIIIIIB", page, 0x200, 0, 0, 1, ces, g["caus_per_ce"] * g["blocks_per_cau"],
                     g["pages_per_block"], g["page_bytes"] // 512, g["meta_bytes"], 7)
    cem = bytearray(44)
    struct.pack_into("<I", cem, 0, 0xdeadcafe)
    struct.pack_into("<H", cem, 42, fletcher16(cem[:42]))
    page[0x224:0x250] = cem
    specs = partition_specs(g, firmware_min_capacity)
    # The pool partitions live in spare blocks: 'rAps' lists the spares as {block:24, ce:8} and 'sbus'
    # maps a pool block's virtual number (its per-chip block number times the chip count, plus the chip,
    # the chip-interleaved numbering the reference parser recovered) to the spare that holds it. The
    # spares are the first blocks after the boot block on every chip, inside the VFL's keepout.
    spares, subs = pool_map(g, specs)
    spare = bytearray(0x10c)
    spare[0:4] = b"rAps"
    struct.pack_into("<H", spare, 4, len(spares))
    for i, (ce, block) in enumerate(spares):
        struct.pack_into("<I", spare, 10 + 4 * i, block | ce << 24)
    struct.pack_into("<H", spare, 0x10a, fletcher16(spare[:0x10a]))
    page[0x250:0x35c] = spare
    sub = bytearray(0x4c)
    sub[0:4] = b"sbus"
    struct.pack_into("<H", sub, 4, len(subs))
    sub[6] = 2
    for i, vblock in enumerate(subs):
        sub[10 + i] = vblock
    struct.pack_into("<H", sub, 0x4a, fletcher16(sub[:0x4a]))
    page[0x35c:0x3a8] = sub
    for i in range(32):
        name, typ, a, b, flags = specs[i] if i < len(specs) else ("none", 0, 0, 1, 0x80000000)
        struct.pack_into("<IIII", page, 0x400 + 16 * i, fourcc(name), a, b, typ | flags)
    return bytes(page)


# ---- the page store -----------------------------------------------------------------------------------

def make_sparse(path, size):
    """A size-byte file of holes. On Windows the sparse flag goes on first and the end is set with
    SetEndOfFile: the C runtime's truncate (what Python's f.truncate is) writes the zeros out."""
    with open(path, "wb") as f:
        if sys.platform != "win32":
            f.truncate(size)
            return
        import msvcrt
        k32 = ctypes.windll.kernel32
        h = ctypes.c_void_p(msvcrt.get_osfhandle(f.fileno()))
        out = ctypes.c_ulong(0)
        if not k32.DeviceIoControl(h, 0x900C4, None, 0, None, 0, ctypes.byref(out), None):   # FSCTL_SET_SPARSE
            raise OSError(ctypes.get_last_error(), f"{path}: cannot set the sparse flag")
        if not k32.SetFilePointerEx(h, ctypes.c_longlong(size), None, 0) or not k32.SetEndOfFile(h):
            raise OSError(ctypes.get_last_error(), f"{path}: cannot set the end of file")


def page_index(g, cau, block, page):
    return (cau * g["blocks_per_cau"] + block) * g["pages_per_block"] + page


def write_page(f, g, cau, block, page, data, meta=None):
    """One page record, XOR 0xff as the model stores it; data shorter than a page leaves the rest erased."""
    stride = g["page_bytes"] + g["meta_bytes"]
    rec = bytearray(data) + b"\xff" * (g["page_bytes"] - len(data))
    rec += bytes(meta or b"") + b"\xff" * (g["meta_bytes"] - len(meta or b""))
    f.seek(page_index(g, cau, block, page) * stride)
    f.write(bytes(b ^ 0xff for b in rec))


def write_store(out_dir, g=PPN, firmware_min_capacity=0x180000, ptab_only=False):
    """A page store with the partition table in its boot blocks and everything else erased; with
    ptab_only, just the table rewritten into an existing store (its FTL and partitions kept)."""
    os.makedirs(out_dir, exist_ok=True)
    if not ptab_only:
        with open(os.path.join(out_dir, "geometry.json"), "w") as f:
            json.dump(g, f, indent=1)
    ptab = build_ptab(g, firmware_min_capacity=firmware_min_capacity)
    stride = g["page_bytes"] + g["meta_bytes"]
    size = g["caus_per_ce"] * g["blocks_per_cau"] * g["pages_per_block"] * stride
    for ch in range(g["channels"]):
        for ce in range(g["ce_per_channel"]):
            path = os.path.join(out_dir, f"ch{ch}-ce{ce}.pages")
            if not ptab_only:
                make_sparse(path, size)
            with open(path, "r+b") as f:
                erase = b"\x00" * stride
                for page in range(g["pages_per_block"]):
                    f.seek(page_index(g, 0, 0, page) * stride)
                    f.write(erase)
                for page in range(2):
                    write_page(f, g, 0, 0, page, ptab)
    return os.path.join(out_dir, "geometry.json")
