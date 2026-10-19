"""The direct-kernel boot bundle (kboot.bin) for the A5 iPhone 4S (n94ap): what iBoot does before xnu.

A port of FirmwareKit's KBoot.swift (LightTouchMac, the builder for qemu-ios' A4 boards) with an A5
board. The bundle is what hw/arm/ipad1.c and the n94 machine load: a flat image of physical memory
(kernel segments, the filled device tree, boot_args), optional "K48SEG" segments, and a 24-byte
trailer {"K48KBOOT", u32 load_pa, entry_pa, bootargs_pa, image_len}. The CPU starts at entry_pa in
ARM state, MMU off, r0 = bootargs_pa.

A5 facts used here (docs/n94/hardware.md): DRAM at 0x80000000, 512 MiB; the 6.1.3 kernelcache links
at 0x80001000, so with no KASLR slide the kernel's VA == PA; timebase 24 MHz; board-id 8, chip-id
0x8940; a 640x960 Retina panel at scale 2.
"""
import hashlib
import lzss
import struct
import zlib
import nand_ppn

from devicetree import DeviceTree, le
from macho import MachO

PRAM_SIZE = 0x4000
VRAM_SIZE = 0x900000 - 0x4000
FB_DEPTH = 32
DEFAULT_BOOT_ARGS = "serial=3 debug=0x8 amfi_allow_any_signature=1 cs_enforcement_disable=1 enable-hsic=1"
ROOT_MATCHING = ("<dict><key>IOProviderClass</key><string>IOMedia</string><key>IOPropertyMatch</key>"
                 "<dict><key>Partition ID</key><integer>1</integer></dict></dict>")
TEST_IMEI = "004999010640000"   # GSMA's test range, as the A4 boards use

# A4 raw-NAND geometry (16 GB, eight dies) the h2fmi model serves; only the keys the DT has are written.
# The PPN geometry (--nand ppn, the N94's own kind of NAND) is nand_ppn.PPN, or a store's own geometry.json
# passed as nand= (a dict): the kernel and the page store must agree on it.
NAND_RAW = [
    ("#ce", 8), ("#die-ce", 1), ("#ce-blocks", 0x1000), ("#block-pages", 128), ("#page-bytes", 4096),
    ("#spare-bytes", 0x80), ("vendor-type", 0x10_0014), ("#databus", 2),
    ("ecc-correctable", 8), ("ecc-threshold", 8), ("bbt-format", 3),
    ("meta-per-logical-page", 12), ("valid-meta-per-logical-page", 10), ("logical-page-size", 4096),
    ("ppn-device", 0), ("ce-bitmap", 0x0F0F),
]


class Board:
    """What iBoot would put in the DT for one board."""

    def __init__(self, name, phys_base, dram, fb, scale, board_id, chip_id, platform, model,
                 radio=True, cpu_hz=800_000_000, mem_hz=400_000_000, bus_hz=200_000_000,
                 periph_hz=100_000_000, fixed_hz=24_000_000, timebase_hz=24_000_000,
                 chip_revision=0x10, lcd_panel_id=0x00A1D13C):
        self.name = name
        self.phys_base, self.dram = phys_base, dram
        self.fb_width, self.fb_height, self.scale = fb[0], fb[1], scale
        self.board_id, self.chip_id, self.platform, self.model = board_id, chip_id, platform, model
        self.radio = radio
        self.cpu_hz, self.mem_hz, self.bus_hz, self.periph_hz = cpu_hz, mem_hz, bus_hz, periph_hz
        self.fixed_hz, self.timebase_hz = fixed_hz, timebase_hz
        self.chip_revision, self.lcd_panel_id = chip_revision, lcd_panel_id

    @property
    def mem_size(self):
        return self.dram - PRAM_SIZE - VRAM_SIZE

    @property
    def vram_pa(self):
        return self.phys_base + self.mem_size

    @property
    def pram_pa(self):
        return self.phys_base + self.dram - PRAM_SIZE

    def clock_table(self, slots):
        """/arm-io/clock-frequencies, the table iBoot fills and a device node's clock-ids (0x100 + slot)
        index. The A4 index map: 0 timebase, 5 cpu, 6 periph, 27 mem, 32 bus, 33 fixed, everything else
        the peripheral clock. The A5's (S5L8940X) slots are typed by AppleS5L8940XIO itself, which names
        each slot it publishes (a table of four names per slot, 10B329 0x809014d0): "nclk" 30 and 50-54,
        "hclk" 31, 33, 36-44 and 63, "pclk" 45-49 (hclk+pclk on 31, 33 and 45-49). The 4S's real values
        come from iBoot's PLL setup and are not known here: slot 0 (the oscillator) and the nclk slots get
        the 24 MHz reference, the hclk slots the bus clock, the rest the peripheral clock. Users in the DT:
        i2c 30, mipi-dsim 32, ae2 33, usb-complex 36, clcd/rgbout/tv-out 38, iop 43, pl310 44, flash 46-47,
        uart/spi1 48 (spi1's function clock 51), pke 49."""
        c = [self.periph_hz] * slots
        if self.platform == "s5l8940x":
            table = {0: self.fixed_hz}
            table.update({i: self.fixed_hz for i in (30, 50, 51, 52, 53, 54)})
            table.update({i: self.bus_hz for i in (31, 33, 36, 37, 38, 39, 40, 41, 42, 43, 44, 63)})
        else:
            table = {0: self.timebase_hz, 5: self.cpu_hz, 6: self.periph_hz, 27: self.mem_hz,
                     32: self.bus_hz, 33: self.fixed_hz}
        for i, hz in table.items():
            if i < slots:
                c[i] = hz
        return c


BOARDS = {
    # iPhone 4S: Restore.plist DeviceMap BDID 8, CPID 0x8940 (35136), platform s5l8940x.
    "N94AP": Board("n94", 0x80000000, 0x20000000, (640, 960), 2, 0x08, 0x8940, "s5l8940x", "MD234"),
    # iPhone 4 GSM, for cross-checks against qemu-ios' own kboot (A4: DRAM at 0x40000000).
    "N90AP": Board("n90", 0x40000000, 0x20000000, (640, 960), 2, 0x00, 0x8930, "s5l8930x", "MC603",
                   cpu_hz=0, mem_hz=0, bus_hz=100_000_000, chip_revision=0x11),
}


def board_of(dt):
    first = dt.strings("", "compatible")[0] if dt.strings("", "compatible") else ""
    if first not in BOARDS:
        raise ValueError(f"no board table for DT compatible {first!r}")
    return BOARDS[first]


def synthesize_identity(seed, board):
    """A plausible unit: serial, MLB, ECID, die-id and MACs derived from the seed (never a real unit's)."""
    h = hashlib.sha256(f"iPhonEmu {board.name} {seed}".encode()).digest()
    alnum = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ"
    pick = lambda n, off: "".join(alnum[h[(off + i) % 32] % len(alnum)] for i in range(n))
    return {
        "serial-number": "C8Q" + pick(9, 0),
        "mlb-serial-number": "C07" + pick(14, 9),
        "unique-chip-id": "0x%016x" % (int.from_bytes(h[0:8], "little") & 0x000FFFFFFFFFFFFF | 0x0001000000000000),
        "die-id": ["0x%08x" % int.from_bytes(h[8:12], "little"), "0x%08x" % int.from_bytes(h[12:16], "little")],
        "wifi-mac": "a4:d1:d2:%02x:%02x:%02x" % tuple(h[16:19]),
        "bt-mac": "a4:d1:d2:%02x:%02x:%02x" % tuple(h[19:22]),
        "model-number": board.model,
        "region-info": "LL/A",
    }


def nvram_image(size):
    """An empty CHRP NVRAM as IODTNVRAM parses it: a 2 KiB "common" partition, the rest "free" (for the
    boards whose NVRAM the kernel only ever sees through this proxy; see fill_dt)."""
    def part(sig, name, units):
        h = bytearray([sig, 0, units & 0xFF, units >> 8]) + name.encode() + b"\0" * (12 - len(name))
        c = h[0]
        for x in h[2:]:
            c += x
            if c > 0xFF:
                c = (c & 0xFF) + 1
        h[1] = c
        return bytes(h) + b"\0" * (units * 16 - 16)
    common = 0x80
    return part(0x70, "common", common) + part(0x7F, "free", size // 16 - common)


def iboot_version(iboot):
    """The first "iBoot-N.N..." in a decrypted iBoot, else a 6.1.3 n94 default."""
    if iboot:
        i = iboot.find(b"iBoot-")
        while i >= 0:
            j = i + 6
            if j < len(iboot) and 48 <= iboot[j] <= 57:
                while j < len(iboot) and (48 <= iboot[j] <= 57 or iboot[j] == 46):
                    j += 1
                return iboot[i:j].rstrip(b".").decode()
            i = iboot.find(b"iBoot-", i + 1)
    return "iBoot-1537.9.55"


def product_id_from_iboot(iboot):
    """/product/product-id as iBoot carries it: the 20 bytes just ahead of "DEVICEINFOBBT" (ipad1.c)."""
    if not iboot:
        return None
    i = iboot.find(b"DEVICEINFOBBT")
    if i < 20:
        return None
    pid = iboot[i - 20:i]
    return pid if any(pid) else None


def mac_bytes(s):
    return bytes.fromhex(s.replace(":", ""))


def fill_dt(dt, board, memory_map, identity, iboot_ver, root_matching, cpus=2, nand="raw",
            product_id=None, hide=()):
    # root: platform, unit identity
    dt.set("", "platform-name", board.platform)
    for k in ("serial-number", "mlb-serial-number", "model-number", "region-info"):
        dt.set_if("", k, identity[k])
    # chosen: a production, secure-booted, trusted unit with its keys available
    for k in ("debug-enabled", "production-cert", "secure-boot", "gid-aes-key", "uid-aes-key", "system-trusted"):
        dt.set_if("chosen", k, 1)
    ecid = int(identity["unique-chip-id"], 16)
    dt.set("chosen", "board-id", board.board_id)
    dt.set("chosen", "chip-id", board.chip_id)
    dt.set("chosen", "unique-chip-id", [ecid & 0xFFFFFFFF, ecid >> 32])
    dt.set_if("chosen", "die-id", [int(w, 16) for w in identity["die-id"]])
    dt.set("chosen", "firmware-version", iboot_ver)
    dt.set_if("chosen", "display-rotation", 0)
    dt.set_if("chosen", "display-scale", board.scale)
    dt.set("chosen", "root-matching", root_matching)
    # NVRAM: iBoot hands the kernel its image as chosen/nvram-proxy-data and IODTNVRAM then never reads
    # the controller (registerNVRAMController skips the read when proxied; it only writes the image back
    # on sync). A board whose NVRAM is on the NAND (IOFlashNVRAM, --nand ppn) must not get a seed: the
    # seed would replace what the last boot saved, every boot, and lacks the 0x5a "nvram" header
    # IOFlashNVRAM validates. Without the property the kernel reads the image from the controller.
    n = dt.slot_len("chosen", "nvram-proxy-data")
    if n and nand != "raw":
        dt.remove("chosen", "nvram-proxy-data")
    elif n:
        dt.set("chosen", "nvram-proxy-data", nvram_image(n))
    if product_id and dt.slot_len("product", "product-id") == len(product_id):
        dt.set("product", "product-id", product_id)
    for k, mac in (("mac-address-wifi0", identity["wifi-mac"]), ("mac-address-bluetooth0", identity["bt-mac"])):
        dt.set_if("chosen", k, mac_bytes(mac))
    # cpus
    for k, hz in (("clock-frequency", board.cpu_hz), ("memory-frequency", board.mem_hz),
                  ("bus-frequency", board.bus_hz), ("peripheral-frequency", board.periph_hz),
                  ("fixed-frequency", board.fixed_hz), ("timebase-frequency", board.timebase_hz)):
        dt.set_if("cpus/cpu0", k, hz)
    if cpus == 1 and dt.contains("cpus/cpu1"):
        # Bring-up on one core: the second CPU's node matches nothing and is not a "cpu". (The boot-args
        # also carry cpus=1, see build(): AppleARMPE counts every child of /cpus for the CPU interrupt
        # controller and only the boot-arg lowers that, and AppleInterruptController::start blocks in
        # IOCPUInterruptController::registerInterrupt until that many CPUs are up.)
        dt.unmatch("cpus/cpu1", "xcpu")
    # arm-io clocks
    slots = (dt.slot_len("arm-io", "clock-frequencies") or 0) // 4
    if slots:
        dt.set("arm-io", "clock-frequencies", board.clock_table(slots))
    dt.set_if("arm-io", "usbphy-frequency", board.fixed_hz)
    dt.set_if("arm-io", "chip-revision", board.chip_revision)
    # devices with no model yet are hidden from IOKit
    if dt.contains("arm-io/sgx"):
        dt.set("arm-io/sgx", "compatible", "none")
    for node in hide:
        if dt.contains(node):
            dt.unmatch(node)
    # MACs on the radios
    for node, mac in (("arm-io/usb-complex/usb-ehci/wlan", identity["wifi-mac"]),
                      ("arm-io/sdio", identity["wifi-mac"]),
                      ("arm-io/uart3/bluetooth", identity["bt-mac"]), ("arm-io/uart1/bluetooth", identity["bt-mac"])):
        if dt.contains(node):
            dt.set_if(node, "local-mac-address", mac_bytes(mac))
    # the panel id iBoot's pinot_init writes (the DSI model's reply)
    for k in ("lcd-panel-id", "raw-panel-id"):
        dt.set_if("arm-io/mipi-dsim/lcd", k, board.lcd_panel_id)
    # baseband: a radio board keeps its node (the machine unmatches it at boot); its identity is a test one
    if dt.contains("baseband"):
        if not board.radio:
            dt.unmatch("baseband", "nobb")
        if dt.has("baseband", "device-imei"):
            dt.set("baseband", "device-imei", TEST_IMEI)
    # NAND geometry
    for node in ("arm-io/flash-controller0", "arm-io/flash-controller0/disk"):
        if not dt.contains(node):
            continue
        if nand == "raw":
            for k, v in NAND_RAW:
                dt.set_if(node, k, v)
        else:
            for k, v in nand_ppn.dt_props(nand if isinstance(nand, dict) else nand_ppn.PPN):
                dt.set_if(node, k, v)
    # memory layout
    dt.set("pram", "reg", [board.pram_pa, PRAM_SIZE])
    dt.set("vram", "reg", [board.vram_pa, VRAM_SIZE])
    dt.set_if("memory", "reg", [board.phys_base, board.mem_size])
    for i, (name, pa, size) in enumerate(memory_map):
        dt.rename("chosen/memory-map", f"MemoryMapReserved-{i}", name)
        dt.set("chosen/memory-map", name, [pa, size])


def build(kernel, devicetree, identity, boot_args=DEFAULT_BOOT_ARGS, iboot=None, cpus=2, nand="raw",
          hide=(), board=None, ramdisk=None):
    """(image bytes, load_pa, entry_pa, bootargs_pa, info dict) for a kernelcache Mach-O and a DT blob.

    `ramdisk` (a raw HFS+ image, the IPSW's decrypted restore or update ramdisk) is staged right after
    boot_args, under topOfKernelData so the kernel does not reclaim it, and published as the
    chosen/memory-map "RAMDisk" entry the kernel's md driver looks up; `rd=md0` makes it the root.
    That is how iBoot boots a restore ramdisk, and it gives launchd a root with no NAND model.
    """
    page = lambda n: (n + 0xFFF) & ~0xFFF
    m = MachO(kernel)
    segs = [s for s in m.segments if s.name != "__PAGEZERO"]
    lowest = min(s.vmaddr for s in segs)
    end = max(s.vmaddr + s.vmsize for s in segs)
    vbase = lowest & 0xF0000000
    dt = DeviceTree(devicetree)
    board = board or board_of(dt)
    pa = lambda va: (va - vbase + board.phys_base) & 0xFFFFFFFF
    if dt.contains("arm-io/usb-complex") and not dt.has("arm-io/usb-complex", "hsic-enabled"):
        dt.add("arm-io/usb-complex", "hsic-enabled")
    dt_len = len(dt.data)
    top = page(end)
    dt_va = top
    args_va = dt_va + page(dt_len)
    end_va = args_va + 0x1000
    if cpus == 1 and "cpus=" not in boot_args:
        boot_args += " cpus=1"
    rd_va, rd_len = end_va, len(ramdisk) if ramdisk else 0
    if ramdisk:
        end_va = rd_va + page(rd_len)
        if "rd=" not in boot_args:
            boot_args += " rd=md0"
    top_of_kernel = pa((end_va + 0x3FFF) & ~0x3FFF)

    image = bytearray(end_va - vbase)
    memory_map = []
    for s in segs:
        n = min(s.filesize, s.vmsize)
        if s.fileoff + n > len(kernel):
            raise ValueError(f"kernelcache segment {s.name} runs past the file")
        at = s.vmaddr - vbase
        image[at:at + n] = kernel[s.fileoff:s.fileoff + n]
        memory_map.append((f"Kernel-{s.name}", pa(s.vmaddr), s.vmsize))
    memory_map += [("DeviceTree", pa(dt_va), dt_len), ("BootArgs", pa(args_va), 0x1000)]
    if ramdisk:
        image[rd_va - vbase:rd_va - vbase + rd_len] = ramdisk
        memory_map.append(("RAMDisk", pa(rd_va), rd_len))
    fill_dt(dt, board, memory_map, identity, iboot_version(iboot), ROOT_MATCHING, cpus=cpus, nand=nand,
            product_id=product_id_from_iboot(iboot), hide=hide)
    if len(dt.data) > dt_len:
        raise RuntimeError("device tree grew after layout")
    # a property fill_dt removed leaves zeros at the end of the slot, past the tree the kernel walks
    image[dt_va - vbase:dt_va - vbase + dt_len] = bytes(dt.data).ljust(dt_len, b"\0")

    # boot_args: rev 1, Version as the kernel checks it; video {base, display, rowbytes, w, h, depth}.
    verbose = any(a in ("-v", "-s") for a in boot_args.split())
    cmdline = boot_args.encode()
    if len(cmdline) >= 256:
        raise ValueError("boot-args longer than BOOT_LINE_LENGTH")
    version = m.boot_args_version()
    ba = bytes([1, 0, version, 0]) + le([
        vbase, board.phys_base, board.mem_size, top_of_kernel,
        board.vram_pa, 0 if verbose else 1, board.fb_width * FB_DEPTH // 8, board.fb_width,
        board.fb_height, FB_DEPTH | (board.scale - 1) << 16,
        0, dt_va, dt_len,
    ]) + cmdline + b"\0" * (256 - len(cmdline))
    image[args_va - vbase:args_va - vbase + len(ba)] = ba
    info = {
        "board": board.name, "virt_base": vbase, "phys_base": board.phys_base, "mem_size": board.mem_size,
        "top_of_kernel": top_of_kernel, "entry": pa(m.entry()), "boot_args_pa": pa(args_va),
        "boot_args_version": version, "device_tree": [pa(dt_va), dt_len], "vram": [board.vram_pa, VRAM_SIZE],
        "pram": [board.pram_pa, PRAM_SIZE], "memory_map": memory_map, "boot_args": boot_args, "cpus": cpus,
        "nand": nand, "ramdisk": [pa(rd_va), rd_len] if ramdisk else None,
    }
    return bytes(image), board.phys_base, pa(m.entry()), pa(args_va), info


def bundle(image, load_pa, entry_pa, bootargs_pa, segments=()):
    out = bytearray(image)
    for seg_pa, length, data in segments:
        out += b"K48SEG\0\0" + le([seg_pa, length, 0 if data is not None else 1])
        if data is not None:
            out += data
    out += b"K48KBOOT" + le([load_pa, entry_pa, bootargs_pa, len(image)])
    return bytes(out)


def iboot_image(blob):
    """(width, height, rows of 8-bit grey over black) of an iBootIm image (the IPSW's AppleLogo once its
    IMG3 is decrypted): "iBootIm\\0", Adler-32 of everything from +12, compression 'lzss', format 'grey',
    u16 width and height, the LZSS stream at +0x40. A 'grey' pixel is two bytes, the level and 255 minus
    its coverage; iBoot composites it over the black panel."""
    if len(blob) < 0x40 or blob[:8] != b"iBootIm\0":
        raise ValueError("not an iBootIm image")
    adler, comp, fmt, width, height = struct.unpack_from("<I4s4sHH", blob, 8)
    if zlib.adler32(blob[12:]) != adler:
        raise ValueError("iBootIm image fails its Adler-32")
    if comp[::-1] != b"lzss" or fmt[::-1] != b"grey":
        raise ValueError(f"iBootIm image is {comp[::-1]!r}/{fmt[::-1]!r}, only lzss/grey is known")
    px = lzss.decompress(blob[0x40:], width * height * 2)
    if len(px) != width * height * 2:
        raise ValueError("iBootIm image is short")
    return width, height, [bytes(px[(y * width + x) * 2] * (255 - px[(y * width + x) * 2 + 1]) // 255
                                 for x in range(width)) for y in range(height)]


def logo_segment(board, blob):
    """The boot logo as iBoot leaves it on the panel: the image centered on the black framebuffer at
    board.vram_pa (BGRA, the rows the logo spans), as a K48SEG segment (pa, length, data)."""
    width, height, rows = iboot_image(blob)
    if width > board.fb_width or height > board.fb_height:
        raise ValueError(f"boot logo {width}x{height} is larger than the {board.fb_width}x{board.fb_height} panel")
    stride = board.fb_width * FB_DEPTH // 8
    x0, y0 = (board.fb_width - width) // 2, (board.fb_height - height) // 2
    band = bytearray(b"\0\0\0\xff" * board.fb_width * height)
    for y, row in enumerate(rows):
        at = y * stride + x0 * 4
        band[at:at + width * 4] = b"".join(bytes((v, v, v, 0xff)) for v in row)
    return board.vram_pa + y0 * stride, len(band), bytes(band)


def parse_bundle(blob):
    """(load_pa, entry_pa, bootargs_pa, image_len) of a K48KBOOT bundle."""
    if len(blob) < 24 or blob[-24:-16] != b"K48KBOOT":
        raise ValueError("not a K48KBOOT bundle")
    return struct.unpack("<4I", blob[-16:])
