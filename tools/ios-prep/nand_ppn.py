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
import json
import os

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
        ("mlc-pages", g["pages_per_block"]), ("match-oddeven-caus", 0), ("max-transaction-size", 0),
        ("vendor-type", 0), ("bbt-format", 0), ("ecc-threshold", 0), ("ecc-correctable", 0),
        ("toggle-device", 0), ("banks-per-ce", 1),
        ("device-readid", chip_id + b"\0\0"), ("dbg-chipids", (chip_id + b"\0\0") * ces),
    ]


def write_store(out_dir, g=PPN):
    """A blank page store: geometry.json alone; the model creates the sparse page files (all erased)."""
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "geometry.json"), "w") as f:
        json.dump(g, f, indent=1)
    return os.path.join(out_dir, "geometry.json")
