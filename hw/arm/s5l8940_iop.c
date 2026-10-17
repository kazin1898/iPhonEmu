/*
 * S5L8940X ("A5") IOP: high-level emulation of the second core's host-visible
 * surface, the shape of hw/arm/s5l8930_iop.c on the A5's interface. The
 * firmware the kernel uploads (IOP_s5l8940xiop_firmware: "EmbeddedIOP for
 * s5l8940xiop", iBoot-1537.9.55) never executes here; this device answers what
 * AppleA5IOP, EmbeddedIOP-47.3 and AppleIOPFMI can observe of it.
 *
 * What the 10B329 kexts and the firmware image say (docs/n94/hardware.md "The
 * IOP"):
 *
 * - The iop node's one register window (0x3F300000): AppleA5IOP::startCPU
 *   writes +0x00 <- 0, +0x04 <- the firmware's physical base, +0x34 <-
 *   0x030001ff (0x010001ff when iop-version is not 1), +0x00 <- 1; stop writes
 *   +0x00 <- 0. No size, SELF or cache registers (the A4 had them).
 * - The AP rings the IOP through the AIC: the node's function-doorbell is the
 *   AIC's "IPID" function with argument 3 (function-nmi: 0x10002), which is
 *   SW_SET of vector 3; the AIC model pulses its "doorbell" output on that
 *   write. The IOP answers on AIC vector 2 (the node's interrupts), a software
 *   vector the AP's handler clears after dispatch: s5l8940_aic_sw_set.
 * - The firmware's 'cnfg' block is at image offset 0x14000 (the kext's
 *   ConfigurationOffset): +0x8 the message buffer, +0x10 + 8 h the ring
 *   {address, count} of endpoint h, the EmbeddedIOP-20+ layout the A4 core
 *   runs; ring entries are 64 bytes, word 0 = item | ownership bit (the IOP
 *   is direction 0: a slot is pending when bit 0 is clear and the answer is
 *   the same item with bit 0 set, in place). The task table names the
 *   endpoints: 0 "iop" (control), 3 "sdio", 5 "fmi0", 6 "fmi1"; 1 is the
 *   message channel (IOP -> AP only).
 * - Control messages are 32 bytes {opcode, status, ...}: 'nop ' (the startup
 *   ping EmbeddedIOP panics without), 'slep', 'spnd', 'rsum', 'ttin'; the AP
 *   workloop writes 'done' over a returned opcode.
 * - FMI commands are 512 bytes, opcode at +0, status at +8, the arguments 8
 *   bytes further up than the A4 firmware's (the 10B329 set_config has its bus
 *   at +0x18): the raw opcodes 1-12, 20, 21 and the PPN ones 2-6, 10, 11, 13,
 *   15-19, 21-26. The N94's NAND is PPN (the kernel has only AppleSwissPPNFTL),
 *   served here from a page store (below); what is not modeled yet is traced
 *   and refused as unimplemented (0x80000005).
 *
 * Addresses in the rings and commands are the IOP's: EmbeddedIOP's visible
 * memory puts DRAM at 0xc0000000 in the IOP's map (the A4 did the same for
 * its DRAM at 0x40000000), so an IOP address is the AP's physical address
 * plus "window-offset" (0x40000000 here; seen in the first ring table the
 * kext handed over: 0xc2438000 for 0x82438000). Anything outside that window
 * is taken as a physical address and logged once.
 *
 * The PPN page store ("nand" property, a directory; tools/ios-prep nand
 * writes one): geometry.json (tools/ios-prep/nand_ppn.py's keys) and one
 * sparse file per chip, ch<channel>-ce<ce>.pages, a record of page_bytes +
 * meta_bytes per page at index (cau * blocks_per_cau + block) * pages_per_block
 * + page, stored XOR 0xff so a hole reads as an erased page (the A4 store's
 * "nand-xor-ff-v2" representation). A row address is (cau << (block_bits +
 * page_bits)) | (block << page_bits) | page; a boot page is the first
 * boot_page_bytes of a page's data.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/s5l8940.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/runstate.h"
#include "block/block.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qobject/qjson.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include "trace.h"
#include "hw/trace-printf.h"

OBJECT_DECLARE_SIMPLE_TYPE(S5L8940IOPState, S5L8940_IOP)

#define IOP_REG_CTRL        0x00
#define IOP_REG_FW_BASE     0x04
#define IOP_REG_CFG         0x34
#define IOP_NREGS           (0x40 / 4)
#define IOP_CTRL_RUN        1

#define FW_CONFIG_MAGIC     0x636e6667  /* 'cnfg' */
#define FW_CONFIG_OFF       0x14000     /* the kext's ConfigurationOffset for this build */
#define FW_HDR_BSS_START    0x318
#define FW_HDR_BSS_END      0x31c
#define FW_MAX_SIZE         0x100000
#define FW_CFG_MSGBUF       0x8
#define FW_CFG_RINGS        0x10
#define FW_CFG_RING(h)      (FW_CFG_RINGS + 8 * (h))
#define FW_CFG_COUNT(h)     (FW_CFG_RINGS + 4 + 8 * (h))
#define IOP_MAX_ENDPOINTS   8
#define IOP_MAX_RING        1024
#define RING_ENTRY_SIZE     64
#define RING_OWNER_AP       1
#define RING_ITEM(w0)       ((w0) & ~3u)

#define RING_CONTROL        0
#define RING_MESSAGE        1
#define RING_SDIO           3
#define RING_FMI0           5
#define RING_FMI1           6

#define CTRL_OP_SLEP        0x736c6570  /* 'slep' */
#define CTRL_OP_NOP         0x6e6f7020  /* 'nop ' */
#define CTRL_OP_RSUM        0x7273756d  /* 'rsum' */
#define CTRL_OP_SPND        0x73706e64  /* 'spnd' */
#define CTRL_OP_TTIN        0x7474696e  /* 'ttin' */
#define CTRL_DONE           0x646f6e65  /* 'done' */

/* FMI command (512 bytes) */
#define CMD_SIZE            512
#define CMD_OPCODE          0x0
#define CMD_FLAGS           0x4
#define CMD_STATUS          0x8
#define CMD_ARG(n)          (0x18 + 4 * (n))    /* the first argument word of every opcode */
#define CFG_IS_PPN          0x88                /* set_config: a PPN channel */
#define FMI_OP_SET_CONFIG   1
#define FMI_OP_RESET_ALL    2
#define FMI_OP_WRITE_BOOTPAGE 10
#define FMI_OP_READ_BOOTPAGE  11
#define FMI_OP_PPN_READ_BBT 13
#define FMI_OP_PPN_READ     16
#define FMI_OP_PPN_ERASE    17
#define FMI_OP_PPN_WRITE    18
#define FMI_OP_PPN_CONTROLLER_INFO 22
#define FMI_OP_PPN_FAILURE_INFO 23
#define FMI_OP_PPN_POWER_STATE 24
#define FMI_OP_PPN_TEMPERATURE 25
#define FMI_OP_PPN_SET_DEBUG_DATA 26
#define CE_INDEX(ce)        ((ce) & 7)          /* commands name a chip FMI1:CE8 style */
#define FMI_STATUS_OK       1
#define FMI_STATUS_BLANK    2
#define FMI_STATUS_UNIMPLEMENTED 0x80000005
#define FMI_STATUS_INVALID  0x80000004
#define SDIO_OP_PING        1
#define SDIO_STATUS_UNKNOWN 2

/* The page store */
#define NAND_MAX_CHANNELS   2
#define NAND_MAX_CE         8
#define NAND_MAX_PAGE       (16384 + 256)
#define NAND_ID_BYTES       8

typedef struct NandGeometry {
    uint32_t channels, ce_per_channel, caus_per_ce, blocks_per_cau, pages_per_block;
    uint32_t page_bytes, meta_bytes, boot_page_bytes, lpn_bytes;
    uint32_t cau_bits, block_bits, page_bits;
    uint8_t chip_id[NAND_ID_BYTES];
    uint32_t stride;            /* page_bytes + meta_bytes */
    uint64_t pages_per_ce;
} NandGeometry;

struct S5L8940IOPState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    DeviceState *aic;
    uint32_t ap_vector;
    uint32_t window_offset;     /* IOP address - AP physical address of DRAM */
    char *nand_dir;

    uint32_t regs[IOP_NREGS];
    bool running;
    uint32_t fw_base;
    uint32_t fw_config;         /* 'cnfg' physical address, 0 until found */
    uint32_t ring_rx[IOP_MAX_ENDPOINTS];
    bool said_outside;
    bool is_ppn[NAND_MAX_CHANNELS];
    QEMUTimer *irq_timer;       /* the answer's interrupt, a round trip after the doorbell */

    NandGeometry geo;
    bool have_store;
    BlockBackend *chip[NAND_MAX_CHANNELS][NAND_MAX_CE];
};

static inline uint32_t iop_ldl(hwaddr addr)
{
    return address_space_ldl_le(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, NULL);
}

static inline uint32_t iop_lduw(hwaddr addr)
{
    return address_space_lduw_le(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, NULL);
}

static inline void iop_stl(hwaddr addr, uint32_t val)
{
    address_space_stl_le(&address_space_memory, addr, val, MEMTXATTRS_UNSPECIFIED, NULL);
}

static inline void iop_read(hwaddr addr, void *buf, hwaddr len)
{
    address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
}

static inline void iop_write(hwaddr addr, const void *buf, hwaddr len)
{
    address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
}

/*
 * An address as the AP sees it: the rings and their items are in the IOP's
 * window (DRAM at 0xc0000000), the buffers AppleIOPFMI hands over in commands
 * are physical (the coherent IOP's DMA sees them as the AP does); both go.
 */
static hwaddr iop_pa(S5L8940IOPState *s, uint32_t a)
{
    uint32_t lo = S5L8940_DRAM_BASE + s->window_offset;

    if (a >= lo && a - lo < S5L8940_DRAM_SIZE) {
        return a - s->window_offset;
    }
    if (a >= S5L8940_DRAM_BASE && a - S5L8940_DRAM_BASE < S5L8940_DRAM_SIZE) {
        return a;
    }
    if (!s->said_outside) {
        s->said_outside = true;
        qemu_log_mask(LOG_GUEST_ERROR, "%s: IOP address 0x%08x outside the DRAM window at 0x%08x\n",
                      __func__, a, lo);
    }
    return a;
}

/* ---- the page store ----------------------------------------------------- */

static int64_t geometry_get(QDict *g, const char *key, Error **errp)
{
    QNum *n = qobject_to(QNum, qdict_get(g, key));
    int64_t v;

    if (!n || !qnum_get_try_int(n, &v)) {
        if (!*errp) {
            error_setg(errp, "geometry.json: missing integer \"%s\"", key);
        }
        return -1;
    }
    return v;
}

static BlockBackend *nand_open_backend(DeviceState *owner, const char *path, uint64_t size, Error **errp)
{
    struct stat st;
    QDict *options;
    BlockBackend *blk;
    BlockDriverState *bs;
    uint64_t perm = BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE;
    uint64_t shared = BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE_UNCHANGED;
    int fd = open(path, O_RDWR | O_CREAT | O_BINARY, 0644);

    if (fd < 0 || fstat(fd, &st) < 0 || (st.st_size == 0 && ftruncate(fd, size) < 0)) {
        error_setg_errno(errp, errno, "cannot open NAND file %s", path);
        if (fd >= 0) {
            close(fd);
        }
        return NULL;
    }
    if (st.st_size != 0 && (uint64_t)st.st_size != size) {
        error_setg(errp, "%s: NAND file size differs from geometry", path);
        close(fd);
        return NULL;
    }
    close(fd);
    options = qdict_new();
    qdict_put_str(options, "driver", "raw");
    bs = bdrv_open(path, NULL, options, BDRV_O_NO_SHARE | BDRV_O_RDWR | BDRV_O_UNMAP |
                   (runstate_check(RUN_STATE_INMIGRATE) ? BDRV_O_INACTIVE : 0), errp);
    if (!bs) {
        return NULL;
    }
    blk = blk_new(bdrv_get_aio_context(bs), 0, shared);
    if (blk_attach_dev(blk, owner) < 0) {
        error_setg(errp, "%s: NAND backend already has a device owner", path);
        blk_unref(blk);
        bdrv_unref(bs);
        return NULL;
    }
    if (blk_set_perm(blk, perm, shared, errp) < 0 || blk_insert_bs(blk, bs, errp) < 0) {
        blk_detach_dev(blk, owner);
        blk_unref(blk);
        bdrv_unref(bs);
        return NULL;
    }
    bdrv_unref(bs);
    return blk;
}

static void nand_open_store(S5L8940IOPState *s, Error **errp)
{
    NandGeometry *g = &s->geo;
    g_autofree char *path = g_strdup_printf("%s/geometry.json", s->nand_dir);
    g_autofree char *text = NULL;
    g_autoptr(GError) gerr = NULL;
    QDict *d;
    const char *id;
    Error *err = NULL;
    uint32_t ch, ce;

    if (!g_file_get_contents(path, &text, NULL, &gerr)) {
        error_setg(errp, "%s: %s", path, gerr->message);
        return;
    }
    d = qobject_to(QDict, qobject_from_json(text, errp));
    if (!d) {
        return;
    }
    g->channels = geometry_get(d, "channels", &err);
    g->ce_per_channel = geometry_get(d, "ce_per_channel", &err);
    g->caus_per_ce = geometry_get(d, "caus_per_ce", &err);
    g->blocks_per_cau = geometry_get(d, "blocks_per_cau", &err);
    g->pages_per_block = geometry_get(d, "pages_per_block", &err);
    g->page_bytes = geometry_get(d, "page_bytes", &err);
    g->meta_bytes = geometry_get(d, "meta_bytes", &err);
    g->boot_page_bytes = geometry_get(d, "boot_page_bytes", &err);
    g->lpn_bytes = geometry_get(d, "logical_page_bytes", &err);
    g->cau_bits = geometry_get(d, "cau_bits", &err);
    g->block_bits = geometry_get(d, "block_bits", &err);
    g->page_bits = geometry_get(d, "page_bits", &err);
    id = qdict_get_try_str(d, "chip_id");
    if (err) {
        error_propagate(errp, err);
        return;
    }
    if (g_strcmp0(qdict_get_try_str(d, "format"), "ppn-xor-ff-v1") || !id || strlen(id) != 12 ||
        g->channels < 1 || g->channels > NAND_MAX_CHANNELS || g->ce_per_channel < 1 ||
        g->ce_per_channel > NAND_MAX_CE || g->caus_per_ce > 1u << g->cau_bits ||
        g->blocks_per_cau > 1u << g->block_bits || g->pages_per_block > 1u << g->page_bits ||
        g->page_bytes + g->meta_bytes > NAND_MAX_PAGE || g->boot_page_bytes > g->page_bytes ||
        !g->lpn_bytes || g->page_bytes % g->lpn_bytes || g->meta_bytes % (g->page_bytes / g->lpn_bytes)) {
        error_setg(errp, "%s: not a PPN page store this model serves", path);
        return;
    }
    for (ch = 0; ch < 6; ch++) {
        g->chip_id[ch] = g_ascii_xdigit_value(id[2 * ch]) << 4 | g_ascii_xdigit_value(id[2 * ch + 1]);
    }
    g->stride = g->page_bytes + g->meta_bytes;
    g->pages_per_ce = (uint64_t)g->caus_per_ce * g->blocks_per_cau * g->pages_per_block;
    for (ch = 0; ch < g->channels; ch++) {
        for (ce = 0; ce < g->ce_per_channel; ce++) {
            g_autofree char *f = g_strdup_printf("%s/ch%u-ce%u.pages", s->nand_dir, ch, ce);

            s->chip[ch][ce] = nand_open_backend(DEVICE(s), f, g->pages_per_ce * g->stride, errp);
            if (!s->chip[ch][ce]) {
                return;
            }
        }
    }
    s->have_store = true;
}

/*
 * A row address's page index in its chip's file, or -1. The bit above the
 * CAU field is the SLC-mode flag (the partition scheme's pool partitions are
 * addressed with it: page | block << page_bits | 1 << (page + block + cau
 * bits)); an SLC block is the same block with fewer pages.
 */
static int64_t nand_page_index(S5L8940IOPState *s, uint32_t row)
{
    NandGeometry *g = &s->geo;
    uint32_t page = row & ((1u << g->page_bits) - 1);
    uint32_t block = (row >> g->page_bits) & ((1u << g->block_bits) - 1);
    uint32_t cau = (row >> (g->page_bits + g->block_bits)) & ((1u << g->cau_bits) - 1);
    uint32_t slc = 1u << (g->page_bits + g->block_bits + g->cau_bits);

    if ((row & ~(slc - 1) & ~slc) || cau >= g->caus_per_ce ||
        block >= g->blocks_per_cau || page >= g->pages_per_block) {
        return -1;
    }
    return ((int64_t)cau * g->blocks_per_cau + block) * g->pages_per_block + page;
}

static BlockBackend *nand_chip(S5L8940IOPState *s, int ch, uint32_t ce)
{
    if (!s->have_store || ch < 0 || ch >= s->geo.channels || ce >= s->geo.ce_per_channel) {
        return NULL;
    }
    return s->chip[ch][ce];
}

/* The page as on flash (data then meta); true when readable, *erased when all 0xff. */
static bool nand_read_page(S5L8940IOPState *s, int ch, uint32_t ce, uint32_t row, uint8_t *buf, bool *erased)
{
    BlockBackend *blk = nand_chip(s, ch, ce);
    int64_t idx = nand_page_index(s, row);
    uint32_t i;

    if (!blk || idx < 0) {
        return false;
    }
    if (blk_pread(blk, idx * s->geo.stride, s->geo.stride, buf, 0) < 0) {
        return false;
    }
    *erased = true;
    for (i = 0; i < s->geo.stride; i++) {
        buf[i] ^= 0xff;
        if (buf[i] != 0xff) {
            *erased = false;
        }
    }
    return true;
}

/* Program: input zeros clear bits, as flash does (old & input); bytes beyond len are left alone. */
static bool nand_program_page(S5L8940IOPState *s, int ch, uint32_t ce, uint32_t row, const uint8_t *in,
                              uint32_t len)
{
    BlockBackend *blk = nand_chip(s, ch, ce);
    int64_t idx = nand_page_index(s, row);
    g_autofree uint8_t *cur = g_malloc(s->geo.stride);
    uint32_t i;

    if (!blk || idx < 0 || len > s->geo.stride) {
        return false;
    }
    if (blk_pread(blk, idx * s->geo.stride, len, cur, 0) < 0) {
        return false;
    }
    for (i = 0; i < len; i++) {
        cur[i] = cur[i] | (in[i] ^ 0xff);    /* stored XOR 0xff: a cleared bit is a set stored bit */
    }
    return blk_pwrite(blk, idx * s->geo.stride, len, cur, 0) >= 0;
}

static bool nand_erase_block(S5L8940IOPState *s, int ch, uint32_t ce, uint32_t row)
{
    BlockBackend *blk = nand_chip(s, ch, ce);
    int64_t idx = nand_page_index(s, row & ~((1u << s->geo.page_bits) - 1));
    int64_t len = (int64_t)s->geo.pages_per_block * s->geo.stride;
    g_autofree uint8_t *buf = NULL;
    int64_t i;

    if (!blk || idx < 0) {
        return false;
    }
    /* An erased block is a hole; zeroing it again would make it real on a host with no unmap. */
    buf = g_malloc(len);
    if (blk_pread(blk, idx * s->geo.stride, len, buf, 0) < 0) {
        return false;
    }
    for (i = 0; i < len && !buf[i]; i++) {
    }
    if (i == len) {
        return true;
    }
    return blk_pwrite_zeroes(blk, idx * s->geo.stride, len, BDRV_REQ_MAY_UNMAP) >= 0;
}

/* ---- commands ------------------------------------------------------------- */

static void iop_control_message(S5L8940IOPState *s, hwaddr item)
{
    uint32_t op = iop_ldl(item);
    uint32_t st = 0;

    switch (op) {
    case CTRL_OP_NOP:
    case CTRL_OP_RSUM:
    case CTRL_OP_SPND:
    case CTRL_OP_TTIN:
        break;
    case CTRL_OP_SLEP:
        /* The firmware answers first, runs the sleep hooks, then halts. */
        s->running = false;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: unrecognised host opcode 0x%08x\n", __func__, op);
        st = 1;
        break;
    }
    trace_s5l8940_iop_ctrl_msg(op, st);
    iop_stl(item + 4, st);
}

static void iop_sdio_command(S5L8940IOPState *s, hwaddr item)
{
    uint32_t op = iop_ldl(item + CMD_OPCODE);

    /* No SDIO device behind the A5 IOP on the N94 (its Wi-Fi is HSIC USB). */
    qemu_log_mask(LOG_UNIMP, "%s: sdio opcode %u\n", __func__, op);
    iop_stl(item + CMD_STATUS, op == SDIO_OP_PING ? 0 : SDIO_STATUS_UNKNOWN);
}

/*
 * Boot pages (opcodes 10, 11): +0x18 u16 chip enable on this channel, +0x1c
 * the row, +0x20 the 1536-byte buffer, +0x24 an optional buffer the firmware
 * fills with the page's sector status words (h2fmi_ppn_read_bootpage; left
 * untouched here). A blank page reads back as 0xff with status 2.
 */
static uint32_t fmi_bootpage(S5L8940IOPState *s, int ch, hwaddr cmd, bool write)
{
    uint32_t ce = CE_INDEX(iop_lduw(cmd + CMD_ARG(0)));
    uint32_t row = iop_ldl(cmd + CMD_ARG(1));
    hwaddr buf = iop_pa(s, iop_ldl(cmd + CMD_ARG(2)));
    g_autofree uint8_t *page = g_malloc(NAND_MAX_PAGE);
    bool erased;

    if (!s->have_store) {
        return FMI_STATUS_BLANK;
    }
    if (write) {
        memset(page, 0xff, s->geo.stride);
        iop_read(buf, page, s->geo.boot_page_bytes);
        TRACE_PRINTF(trace_s5l8940_iop_log, "bootpage write ch%d ce%u row 0x%08x: %02x%02x%02x%02x\n",
                     ch, ce, row, page[0], page[1], page[2], page[3]);
        return nand_program_page(s, ch, ce, row, page, s->geo.boot_page_bytes) ? FMI_STATUS_OK
                                                                              : FMI_STATUS_INVALID;
    }
    if (!nand_read_page(s, ch, ce, row, page, &erased)) {
        return FMI_STATUS_INVALID;
    }
    iop_write(buf, page, s->geo.boot_page_bytes);
    TRACE_PRINTF(trace_s5l8940_iop_log, "bootpage read ch%d ce%u row 0x%08x: %s %02x%02x%02x%02x\n",
                 ch, ce, row, erased ? "blank" : "data", page[0], page[1], page[2], page[3]);
    return erased ? FMI_STATUS_BLANK : FMI_STATUS_OK;
}

/*
 * get_controller_info (opcode 22, h2fmi_ppn_iop_get_controller_info): +0x18 u16
 * chip enable; the answer is what the firmware reads off the chip with PPN
 * get-feature commands and its device-parameters page: +0x1c firmware version
 * (feature 0x9080, 16 bytes), +0x2c manufacturer ID (0xa080), +0x3c package
 * assembly code (0xa180), +0x4c controller unique ID (0xa181), +0x5c controller
 * hardware ID (8 bytes, read-ID address 0x30), +0x64 marketing name (0x85, 10),
 * +0x70 the device parameters: caus per channel, cau bits, blocks per CAU,
 * block bits, pages per block, SLC pages per block, page address bits,
 * bits-per-cell bits, default bits per cell, page size (data + meta), dies,
 * then tRC tREA tREH tRHOH tRHZ tRLOH tRP tWC tWH tWP (one word each), the read,
 * program and erase queue sizes, the prep-function buffer size, tRST, tPURST,
 * tSCE (ms) and tCERDY (us). h2fmi_ppn_get_device_params bounds every one.
 */
static uint32_t fmi_controller_info(S5L8940IOPState *s, hwaddr cmd)
{
    NandGeometry *g = &s->geo;
    static const char fw[16] = "iPhonEmu PPN 1.0", pkg[16] = "QEMU-PPN-PKG";
    static const char uid[16] = "iPhonEmu-uid-000", hwid[8] = "QEMUPPN1", name[10] = "iPhonEmuND";
    uint8_t mfg[16] = { 0 };
    uint32_t params[] = {
        g->caus_per_ce, g->cau_bits, g->blocks_per_cau, g->block_bits, g->pages_per_block,
        g->pages_per_block / 2, g->page_bits, 1, 2, g->page_bytes + g->meta_bytes, g->caus_per_ce,
        20, 20, 10, 15, 100, 5, 12, 25, 15, 12,         /* timings, ns */
        16, 16, 8, 64, 5, 5, 5, 100,                     /* queues, prep buffer, tRST tPURST tSCE ms, tCERDY us */
    };
    uint32_t i;

    if (!s->have_store) {
        return FMI_STATUS_INVALID;
    }
    memcpy(mfg, g->chip_id, 6);
    iop_write(cmd + 0x1c, fw, 16);
    iop_write(cmd + 0x2c, mfg, 16);
    iop_write(cmd + 0x3c, pkg, 16);
    iop_write(cmd + 0x4c, uid, 16);
    iop_write(cmd + 0x5c, hwid, 8);
    iop_write(cmd + 0x64, name, 10);
    for (i = 0; i < ARRAY_SIZE(params); i++) {
        iop_stl(cmd + 0x70 + 4 * i, params[i]);
    }
    return FMI_STATUS_OK;
}

/*
 * read_cau_bbt (opcode 13): the PPN_COMMAND at +0x2c names the chip and CAU;
 * +0x1c points at a word holding the address of a blocks_per_cau / 8 byte
 * bitmap the firmware fills from the chip. No block of this store is bad.
 */
static uint32_t fmi_read_cau_bbt(S5L8940IOPState *s, int ch, hwaddr cmd)
{
    hwaddr bbt = iop_pa(s, iop_ldl(iop_pa(s, iop_ldl(cmd + 0x1c))));
    g_autofree uint8_t *ones = NULL;

    if (!s->have_store) {
        return FMI_STATUS_INVALID;
    }
    ones = g_malloc(s->geo.blocks_per_cau / 8);
    memset(ones, 0xff, s->geo.blocks_per_cau / 8);
    iop_write(bbt, ones, s->geo.blocks_per_cau / 8);
    return FMI_STATUS_OK;
}

/*
 * The PPN_COMMAND AppleSwissPPNFTL builds and AppleIOPFMI hands over at +0x2c
 * (+0x30 its length, 0x1c + 12 per page), decoded from the FTL's builder
 * (s_vfl's page-run adder) and the firmware's erase and read handlers: +0x0
 * the FTL's command (1 read, 2 program, 3 erase, 4 CAU bad-block table), +0x4
 * flags, +0x8 u16 pages, +0xa u16 logical pages in all, +0xc u8 the OR of the
 * page statuses (out), +0xe and +0x14 two chip records {u16 chip enable, u16
 * pages, u16 first entry}, then one 12-byte entry per page at +0x1c: {u16
 * bytes (logical pages x (4 KiB + their meta)), u16 column (the first logical
 * page's offset in those units), u32 row, u8 chip record, u8 logical pages, u8
 * status (out), u8}. The FTL works in logical pages: the data comes and goes
 * through the command's segment lists (+0x1c/+0x20 the data list address and
 * its size in bytes, +0x24/+0x28 the meta list), 8-byte {address, length}
 * entries, 4 KiB then 16 bytes of meta per logical page in entry order. A page
 * status is 0x40 done, 0x49 done and clean (erased), 0x10 uncorrectable (sFTL
 * accepts 0x40, 0x42-0x45, 0x49, 0x51 and 0xc1; anything else is "unidentified").
 */
#define PPN_CMD_PAGES       0x8
#define PPN_CMD_STATUS      0xc
#define PPN_CMD_CHIP(k)     (0xe + 6 * (k))
#define PPN_CMD_ENTRY(j)    (0x1c + 12 * (j))
#define PPN_ENTRY_COL       2
#define PPN_ENTRY_ROW       4
#define PPN_ENTRY_CHIP      8
#define PPN_ENTRY_LPAGES    9
#define PPN_ENTRY_STATUS    10
#define PPN_MAX_PAGES       128
#define PPN_ST_DONE         0x40
#define PPN_ST_CLEAN        0x09       /* the FTL takes 0x49 as "done, clean" (0x48 is unidentified) */
#define PPN_ST_ERROR        0x10

typedef struct SegCursor {
    hwaddr list, end;       /* the {addr, len} entries */
    hwaddr addr;            /* inside the current segment */
    uint32_t left;
} SegCursor;

static void seg_cursor_init(S5L8940IOPState *s, SegCursor *c, hwaddr cmd, int off)
{
    c->list = iop_pa(s, iop_ldl(cmd + off));
    c->end = c->list + iop_ldl(cmd + off + 4);
    c->addr = 0;
    c->left = 0;
}

/* Copy len bytes through the list, to or from buf; short lists leave buf as it is. */
static void seg_copy(S5L8940IOPState *s, SegCursor *c, uint8_t *buf, uint32_t len, bool to_guest)
{
    while (len) {
        uint32_t n;

        if (!c->left) {
            if (c->list + 8 > c->end) {
                return;
            }
            c->addr = iop_pa(s, iop_ldl(c->list));
            c->left = iop_ldl(c->list + 4);
            c->list += 8;
            continue;
        }
        n = MIN(len, c->left);
        if (to_guest) {
            iop_write(c->addr, buf, n);
        } else {
            iop_read(c->addr, buf, n);
        }
        buf += n;
        c->addr += n;
        c->left -= n;
        len -= n;
    }
}

static inline uint8_t iop_ldub(hwaddr addr)
{
    return address_space_ldub(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, NULL);
}

static uint32_t fmi_ppn_command(S5L8940IOPState *s, int ch, hwaddr cmd, uint32_t op)
{
    NandGeometry *g = &s->geo;
    hwaddr pc = iop_pa(s, iop_ldl(cmd + 0x2c));
    uint32_t n = iop_lduw(pc + PPN_CMD_PAGES);
    uint32_t chip_ce[2] = { iop_lduw(pc + PPN_CMD_CHIP(0)), iop_lduw(pc + PPN_CMD_CHIP(1)) };
    uint32_t lpn_bytes = g->lpn_bytes, lpns = g->page_bytes / lpn_bytes, lpn_meta = g->meta_bytes / lpns;
    g_autofree uint8_t *page = g_malloc(NAND_MAX_PAGE);
    SegCursor data, meta;
    uint8_t overall = 0;
    uint32_t j, k;

    if (!s->have_store || n == 0 || n > PPN_MAX_PAGES) {
        return FMI_STATUS_INVALID;
    }
    seg_cursor_init(s, &data, cmd, 0x1c);
    seg_cursor_init(s, &meta, cmd, 0x24);
    for (j = 0; j < n; j++) {
        hwaddr e = pc + PPN_CMD_ENTRY(j);
        uint32_t row = iop_ldl(e + PPN_ENTRY_ROW);
        uint32_t ce = CE_INDEX(chip_ce[iop_ldub(e + PPN_ENTRY_CHIP) & 1]);
        uint32_t first = iop_lduw(e + PPN_ENTRY_COL) / (lpn_bytes + lpn_meta);
        uint32_t count = iop_ldub(e + PPN_ENTRY_LPAGES);
        uint8_t st;
        bool erased = false;

        if (first + count > lpns) {
            count = first < lpns ? lpns - first : 0;
        }
        switch (op) {
        case FMI_OP_PPN_READ:
            if (nand_read_page(s, ch, ce, row, page, &erased)) {
                for (k = first; k < first + count; k++) {
                    seg_copy(s, &data, page + k * lpn_bytes, lpn_bytes, true);
                    seg_copy(s, &meta, page + g->page_bytes + k * lpn_meta, lpn_meta, true);
                }
                st = erased ? PPN_ST_DONE | PPN_ST_CLEAN : PPN_ST_DONE;
            } else {
                st = PPN_ST_ERROR;
            }
            break;
        case FMI_OP_PPN_WRITE:
            memset(page, 0xff, g->stride);
            for (k = first; k < first + count; k++) {
                seg_copy(s, &data, page + k * lpn_bytes, lpn_bytes, false);
                seg_copy(s, &meta, page + g->page_bytes + k * lpn_meta, lpn_meta, false);
            }
            st = nand_program_page(s, ch, ce, row, page, g->stride) ? PPN_ST_DONE : PPN_ST_ERROR;
            break;
        default:
            st = nand_erase_block(s, ch, ce, row) ? PPN_ST_DONE : PPN_ST_ERROR;
            break;
        }
        TRACE_PRINTF(trace_s5l8940_iop_log, "ppn %s ch%d ce%u row 0x%08x lpn %u+%u -> %02x\n",
                     op == FMI_OP_PPN_READ ? "read" : op == FMI_OP_PPN_WRITE ? "write" : "erase",
                     ch, ce, row, first, count, st);
        address_space_write(&address_space_memory, e + PPN_ENTRY_STATUS, MEMTXATTRS_UNSPECIFIED, &st, 1);
        overall |= st;
    }
    address_space_write(&address_space_memory, pc + PPN_CMD_STATUS, MEMTXATTRS_UNSPECIFIED, &overall, 1);
    iop_stl(cmd + 0x64, overall);
    return (overall & PPN_ST_ERROR) ? 0x80000020 : FMI_STATUS_OK;
}

/*
 * The single-page opcodes the partition scheme's clients use (IOFlashNVRAM,
 * AppleEffaceableNAND, AppleNANDFirmware), h2fmi_ppn_iop_{erase,read,write}_single:
 * +0x18 u16 chip enable, +0x1c the block (opcode 3, shifted by the page bits)
 * or the row (4, 6), +0x20 the data buffer and +0x24 the meta buffer, both
 * physical and whole-page. Opcode 5 is the raw read the firmware answers with
 * 0xff over data and meta.
 */
#define FMI_OP_ERASE_SINGLE 3
#define FMI_OP_READ_SINGLE  4
#define FMI_OP_READ_RAW     5
#define FMI_OP_WRITE_SINGLE 6

static uint32_t fmi_single(S5L8940IOPState *s, int ch, hwaddr cmd, uint32_t op)
{
    NandGeometry *g = &s->geo;
    uint32_t ce = CE_INDEX(iop_lduw(cmd + CMD_ARG(0)));
    uint32_t row = iop_ldl(cmd + CMD_ARG(1));
    hwaddr data = iop_pa(s, iop_ldl(cmd + CMD_ARG(2)));
    hwaddr meta = iop_pa(s, iop_ldl(cmd + CMD_ARG(3)));
    g_autofree uint8_t *page = g_malloc(NAND_MAX_PAGE);
    bool erased = false, ok;

    if (!s->have_store) {
        return FMI_STATUS_INVALID;
    }
    switch (op) {
    case FMI_OP_ERASE_SINGLE:
        row <<= g->page_bits;
        ok = nand_erase_block(s, ch, ce, row);
        break;
    case FMI_OP_READ_SINGLE:
        ok = nand_read_page(s, ch, ce, row, page, &erased);
        if (ok) {
            iop_write(data, page, g->page_bytes);
            iop_write(meta, page + g->page_bytes, g->meta_bytes);
        }
        break;
    case FMI_OP_READ_RAW:
        memset(page, 0xff, g->stride);
        iop_write(data, page, g->stride);
        ok = true;
        break;
    default:
        iop_read(data, page, g->page_bytes);
        iop_read(meta, page + g->page_bytes, g->meta_bytes);
        ok = nand_program_page(s, ch, ce, row, page, g->stride);
        break;
    }
    TRACE_PRINTF(trace_s5l8940_iop_log, "ppn single op %u ch%d ce%u row 0x%08x -> %s%s\n", op, ch, ce, row,
                 ok ? "ok" : "error", erased ? " blank" : "");
    return !ok ? 0x80000020 : erased ? FMI_STATUS_BLANK : FMI_STATUS_OK;
}

/* The PPN_COMMAND (AppleSwissPPNFTL's, at +0x2c/+0x30): traced until it is served. */
static void fmi_trace_ppn_command(S5L8940IOPState *s, int ch, hwaddr cmd)
{
    hwaddr pc = iop_pa(s, iop_ldl(cmd + 0x2c));
    uint32_t len = iop_ldl(cmd + 0x30), i;

    TRACE_PRINTF(trace_s5l8940_iop_log, "ppn command ch%d at 0x%" HWADDR_PRIx " len %u: hdr %08x %08x %08x %08x %08x %08x %08x;"
                 " data %08x+%u meta %08x+%u extra %08x x%u aes %08x %08x %08x\n", ch, pc, len,
                 iop_ldl(pc), iop_ldl(pc + 4), iop_ldl(pc + 8), iop_ldl(pc + 0xc), iop_ldl(pc + 0x10),
                 iop_ldl(pc + 0x14), iop_ldl(pc + 0x18), iop_ldl(cmd + 0x1c), iop_ldl(cmd + 0x20),
                 iop_ldl(cmd + 0x24), iop_ldl(cmd + 0x28), iop_ldl(cmd + 0x34), iop_ldl(cmd + 0x38),
                 iop_ldl(cmd + 0x3c), iop_ldl(cmd + 0x40), iop_ldl(cmd + 0x44));
    for (i = 0; i < 4 && 0x1c + 12 * i + 12 <= len; i++) {
        hwaddr e = pc + 0x1c + 12 * i;
        TRACE_PRINTF(trace_s5l8940_iop_log, "  entry %u: %08x %08x %08x\n", i, iop_ldl(e), iop_ldl(e + 4),
                     iop_ldl(e + 8));
    }
}

static void iop_fmi_command(S5L8940IOPState *s, int ch, hwaddr item)
{
    uint32_t op = iop_ldl(item + CMD_OPCODE);
    uint32_t st;

    switch (op) {
    case FMI_OP_SET_CONFIG:
        s->is_ppn[ch] = iop_ldl(item + CFG_IS_PPN) != 0;
        st = FMI_STATUS_OK;
        break;
    case FMI_OP_READ_BOOTPAGE:
        st = fmi_bootpage(s, ch, item, false);
        break;
    case FMI_OP_WRITE_BOOTPAGE:
        st = fmi_bootpage(s, ch, item, true);
        break;
    case FMI_OP_ERASE_SINGLE:
    case FMI_OP_READ_SINGLE:
    case FMI_OP_READ_RAW:
    case FMI_OP_WRITE_SINGLE:
        st = fmi_single(s, ch, item, op);
        break;
    case FMI_OP_PPN_CONTROLLER_INFO:
        st = fmi_controller_info(s, item);
        break;
    case FMI_OP_PPN_READ_BBT:
        fmi_trace_ppn_command(s, ch, item);
        st = fmi_read_cau_bbt(s, ch, item);
        break;
    case FMI_OP_PPN_FAILURE_INFO: {     /* die info: +0x18 u8 ce, +0x1c die; +0x20 unique ID (16), +0x30 chip ID (8) */
        char uid[20], cid[8];

        snprintf(uid, sizeof(uid), "iPhonEmu-d%u-%02x", iop_ldl(item + CMD_ARG(1)) & 0xff,
                 iop_ldl(item + CMD_ARG(0)) & 0xff);
        memcpy(cid, s->geo.chip_id, 6);
        cid[6] = iop_ldl(item + CMD_ARG(1)) & 0xff;
        cid[7] = 0;
        iop_write(item + CMD_ARG(2), uid, sizeof(uid));
        iop_write(item + CMD_ARG(6), cid, sizeof(cid));
        st = FMI_STATUS_OK;
        break;
    }
    case FMI_OP_PPN_POWER_STATE:        /* every chip enable woken */
    case FMI_OP_PPN_SET_DEBUG_DATA:     /* +0x18 a buffer of +0x1c/16 entries, ignored */
        st = FMI_STATUS_OK;
        break;
    case FMI_OP_PPN_TEMPERATURE:        /* ppn15: +0x18 u16, degrees C + 0x111 */
        iop_stl(item + CMD_ARG(0), 25 + 0x111);
        st = FMI_STATUS_OK;
        break;
    case FMI_OP_PPN_READ:
    case FMI_OP_PPN_WRITE:
    case FMI_OP_PPN_ERASE:
        st = fmi_ppn_command(s, ch, item, op);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: fmi%d opcode %u\n", __func__, ch, op);
        st = FMI_STATUS_UNIMPLEMENTED;
        break;
    }
    trace_s5l8940_iop_fmi_cmd(ch, op, iop_ldl(item + CMD_ARG(0)), iop_ldl(item + CMD_ARG(1)),
                              iop_ldl(item + CMD_ARG(2)), iop_ldl(item + CMD_ARG(3)), st);
    iop_stl(item + CMD_STATUS, st);
}

/* ---- rings ---------------------------------------------------------------- */

/* The firmware's 'cnfg' block: at the kext's offset, else by its magic. */
static hwaddr iop_config(S5L8940IOPState *s)
{
    uint32_t off;

    if (s->fw_config) {
        return s->fw_config;
    }
    if (iop_ldl(s->fw_base + FW_CONFIG_OFF) == FW_CONFIG_MAGIC) {
        s->fw_config = s->fw_base + FW_CONFIG_OFF;
        return s->fw_config;
    }
    for (off = 0; off + 4 <= FW_MAX_SIZE; off += 4) {
        if (iop_ldl(s->fw_base + off) == FW_CONFIG_MAGIC) {
            s->fw_config = s->fw_base + off;
            break;
        }
    }
    return s->fw_config;
}

static int iop_walk_ring(S5L8940IOPState *s, int h, hwaddr ring, uint32_t n)
{
    int served = 0;
    uint32_t i;

    for (i = 0; i < n; i++) {
        hwaddr slot = ring + s->ring_rx[h] * RING_ENTRY_SIZE;
        uint32_t w0 = iop_ldl(slot);
        hwaddr item;

        if ((w0 & 1) == RING_OWNER_AP || RING_ITEM(w0) == 0) {
            break;
        }
        item = iop_pa(s, RING_ITEM(w0));
        switch (h) {
        case RING_CONTROL:
            iop_control_message(s, item);
            break;
        case RING_SDIO:
            iop_sdio_command(s, item);
            break;
        case RING_FMI0:
        case RING_FMI1:
            iop_fmi_command(s, h - RING_FMI0, item);
            break;
        }
        iop_stl(slot, RING_ITEM(w0) | RING_OWNER_AP);
        s->ring_rx[h] = (s->ring_rx[h] + 1) % n;
        served++;
    }
    return served;
}

/*
 * AP -> IOP doorbell: every server task drains its ring. The ring table is
 * re-read from the config block each time because the kext registers the
 * FMI endpoints after the control ring, not necessarily before run.
 */
static void iop_doorbell(void *opaque, int n, int level)
{
    static const int served_rings[] = { RING_CONTROL, RING_SDIO, RING_FMI0, RING_FMI1 };
    S5L8940IOPState *s = opaque;
    hwaddr cfg;
    int served = 0;
    int i;

    if (!level) {
        return;
    }
    if (!s->running) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: doorbell while stopped\n", __func__);
        return;
    }
    cfg = iop_config(s);
    if (!cfg) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: no 'cnfg' block in the firmware at 0x%08x\n",
                      __func__, s->fw_base);
        return;
    }
    for (i = 0; i < ARRAY_SIZE(served_rings); i++) {
        int h = served_rings[i];
        uint32_t ring = iop_ldl(cfg + FW_CFG_RING(h));
        uint32_t count = iop_ldl(cfg + FW_CFG_COUNT(h));

        if (!ring || count == 0) {
            continue;
        }
        if (count > IOP_MAX_RING) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: ring %d has %u entries\n", __func__, h, count);
            continue;
        }
        trace_s5l8940_iop_ring(h, ring, count);
        served += iop_walk_ring(s, h, iop_pa(s, ring), count);
    }
    trace_s5l8940_iop_doorbell(served);
    if (served) {
        /*
         * Deferred by about a real IOP's round trip, as the A4 HLE does: an answer
         * raised inside the store that rang the doorbell interrupts the kext
         * before it has finished posting, and the FTL's format then hung after a
         * few thousand commands waiting for a completion it had already missed.
         */
        timer_mod(s->irq_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100 * SCALE_US);
    }
}

static void iop_irq_expire(void *opaque)
{
    S5L8940IOPState *s = opaque;

    s5l8940_aic_sw_set(s->aic, s->ap_vector);
}

/* CTRL = 1: what the firmware's start() does that the AP can see. */
static void iop_run(S5L8940IOPState *s)
{
    uint32_t bss, end;

    s->fw_base = s->regs[IOP_REG_FW_BASE / 4];
    s->fw_config = 0;
    s->said_outside = false;
    memset(s->ring_rx, 0, sizeof(s->ring_rx));
    if (s->fw_base < S5L8940_DRAM_BASE || s->fw_base >= S5L8940_DRAM_BASE + S5L8940_DRAM_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: firmware base 0x%08x outside DRAM\n", __func__, s->fw_base);
        return;
    }
    bss = iop_ldl(s->fw_base + FW_HDR_BSS_START);
    end = iop_ldl(s->fw_base + FW_HDR_BSS_END);
    if (bss < end && end <= FW_MAX_SIZE) {
        /* bss (PanicString, PanicFunction, PanicLog...) reads as clean. */
        g_autofree uint8_t *zero = g_malloc0(end - bss);
        iop_write(s->fw_base + bss, zero, end - bss);
    }
    s->running = true;
    trace_s5l8940_iop_run(s->fw_base, (uint32_t)iop_config(s));
}

static uint64_t s5l8940_iop_read(void *opaque, hwaddr off, unsigned size)
{
    S5L8940IOPState *s = opaque;

    if (off < sizeof(s->regs)) {
        return s->regs[off / 4];
    }
    qemu_log_mask(LOG_UNIMP, "%s: unmodeled read 0x%04" HWADDR_PRIx "\n", __func__, off);
    return 0;
}

static void s5l8940_iop_write(void *opaque, hwaddr off, uint64_t val64, unsigned size)
{
    S5L8940IOPState *s = opaque;
    uint32_t val = val64;

    trace_s5l8940_iop_reg_write((uint32_t)off, val);
    if (off >= sizeof(s->regs)) {
        qemu_log_mask(LOG_UNIMP, "%s: unmodeled write 0x%04" HWADDR_PRIx " <- 0x%08x\n",
                      __func__, off, val);
        return;
    }
    s->regs[off / 4] = val;
    if (off == IOP_REG_CTRL) {
        if (val == IOP_CTRL_RUN) {
            iop_run(s);
        } else {
            s->running = false;
        }
    }
}

static const MemoryRegionOps s5l8940_iop_ops = {
    .read = s5l8940_iop_read,
    .write = s5l8940_iop_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void s5l8940_iop_reset(DeviceState *dev)
{
    S5L8940IOPState *s = S5L8940_IOP(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ring_rx, 0, sizeof(s->ring_rx));
    memset(s->is_ppn, 0, sizeof(s->is_ppn));
    s->running = false;
    s->fw_base = s->fw_config = 0;
    s->said_outside = false;
    timer_del(s->irq_timer);
}

static void s5l8940_iop_realize(DeviceState *dev, Error **errp)
{
    S5L8940IOPState *s = S5L8940_IOP(dev);

    if (!s->aic) {
        error_setg(errp, "s5l8940.iop needs its aic link");
        return;
    }
    if (s->nand_dir) {
        nand_open_store(s, errp);
        if (*errp) {
            return;
        }
    }
    qdev_init_gpio_in_named(dev, iop_doorbell, "doorbell", 1);
}

static void s5l8940_iop_init(Object *obj)
{
    S5L8940IOPState *s = S5L8940_IOP(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8940_iop_ops, s, TYPE_S5L8940_IOP, S5L8940_IOP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, iop_irq_expire, s);
}

static void s5l8940_iop_finalize(Object *obj)
{
    S5L8940IOPState *s = S5L8940_IOP(obj);
    int ch, ce;

    for (ch = 0; ch < NAND_MAX_CHANNELS; ch++) {
        for (ce = 0; ce < NAND_MAX_CE; ce++) {
            if (s->chip[ch][ce]) {
                blk_detach_dev(s->chip[ch][ce], DEVICE(s));
                blk_unref(s->chip[ch][ce]);
            }
        }
    }
    timer_free(s->irq_timer);
    g_free(s->nand_dir);
}

static const VMStateDescription vmstate_s5l8940_iop = {
    .name = TYPE_S5L8940_IOP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8940IOPState, IOP_NREGS),
        VMSTATE_BOOL(running, S5L8940IOPState),
        VMSTATE_UINT32(fw_base, S5L8940IOPState),
        VMSTATE_UINT32(fw_config, S5L8940IOPState),
        VMSTATE_UINT32_ARRAY(ring_rx, S5L8940IOPState, IOP_MAX_ENDPOINTS),
        VMSTATE_BOOL_ARRAY(is_ppn, S5L8940IOPState, NAND_MAX_CHANNELS),
        VMSTATE_TIMER_PTR(irq_timer, S5L8940IOPState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property s5l8940_iop_props[] = {
    DEFINE_PROP_LINK("aic", S5L8940IOPState, aic, TYPE_S5L8940_AIC, DeviceState *),
    DEFINE_PROP_UINT32("ap-vector", S5L8940IOPState, ap_vector, S5L8940_IRQ_IOP),
    DEFINE_PROP_UINT32("window-offset", S5L8940IOPState, window_offset, 0x40000000),
    DEFINE_PROP_STRING("nand", S5L8940IOPState, nand_dir),
};

static void s5l8940_iop_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, s5l8940_iop_props);
    dc->realize = s5l8940_iop_realize;
    dc->vmsd = &vmstate_s5l8940_iop;
    device_class_set_legacy_reset(dc, s5l8940_iop_reset);
}

static const TypeInfo s5l8940_iop_info = {
    .name          = TYPE_S5L8940_IOP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8940IOPState),
    .instance_init = s5l8940_iop_init,
    .instance_finalize = s5l8940_iop_finalize,
    .class_init    = s5l8940_iop_class_init,
};

static void s5l8940_iop_register_types(void)
{
    type_register_static(&s5l8940_iop_info);
}

type_init(s5l8940_iop_register_types)
