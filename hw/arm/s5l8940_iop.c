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
 *   AIC's "IPID" function with argument 3 (function-nmi: 0x10002). The AIC
 *   model pulses its "doorbell" output when the AP raises that vector; the
 *   firmware would see it as an IACK of type 7 (docs). The IOP answers on AIC
 *   vector 2 (the node's interrupts), a software vector the AP's handler
 *   clears after dispatch: s5l8940_aic_sw_set.
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
 * - FMI commands are 512 bytes, opcode at +0, status at +8: the raw opcodes
 *   1-12, 20, 21 and the PPN ones 2-6, 10, 11, 13, 15-19, 21-26. The N94's NAND
 *   is PPN (the kernel has only AppleSwissPPNFTL); the PPN opcodes and the
 *   page store behind them are the next step (B2), so for now a command is
 *   traced and refused as unimplemented (0x80000005), set_config excepted.
 *
 * Addresses in the rings and commands are the IOP's: EmbeddedIOP's visible
 * memory puts DRAM at 0xc0000000 in the IOP's map (the A4 did the same for
 * its DRAM at 0x40000000), so an IOP address is the AP's physical address
 * plus "window-offset" (0x40000000 here; seen in the first ring table the
 * kext handed over: 0xc2438000 for 0x82438000). Anything outside that window
 * is taken as a physical address and logged once.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/s5l8940.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
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

#define CMD_SIZE            512
#define CMD_OPCODE          0x0
#define CMD_STATUS          0x8
#define FMI_OP_SET_CONFIG   1
#define FMI_STATUS_OK       1
#define FMI_STATUS_UNIMPLEMENTED 0x80000005
#define SDIO_OP_PING        1
#define SDIO_STATUS_UNKNOWN 2

struct S5L8940IOPState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    DeviceState *aic;
    uint32_t ap_vector;
    uint32_t window_offset;     /* IOP address - AP physical address of DRAM */

    uint32_t regs[IOP_NREGS];
    bool running;
    uint32_t fw_base;
    uint32_t fw_config;         /* 'cnfg' physical address, 0 until found */
    uint32_t ring_rx[IOP_MAX_ENDPOINTS];
    bool said_outside;
};

static inline uint32_t iop_ldl(hwaddr addr)
{
    return address_space_ldl_le(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, NULL);
}

static inline void iop_stl(hwaddr addr, uint32_t val)
{
    address_space_stl_le(&address_space_memory, addr, val, MEMTXATTRS_UNSPECIFIED, NULL);
}

/* An IOP address as the AP sees it: DRAM through the IOP's window. */
static hwaddr iop_pa(S5L8940IOPState *s, uint32_t a)
{
    uint32_t lo = S5L8940_DRAM_BASE + s->window_offset;

    if (a >= lo && a - lo < S5L8940_DRAM_SIZE) {
        return a - s->window_offset;
    }
    if (!s->said_outside) {
        s->said_outside = true;
        qemu_log_mask(LOG_GUEST_ERROR, "%s: IOP address 0x%08x outside the DRAM window at 0x%08x\n",
                      __func__, a, lo);
    }
    return a;
}

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

static void iop_fmi_command(S5L8940IOPState *s, int bus, hwaddr item)
{
    uint32_t op = iop_ldl(item + CMD_OPCODE);
    uint32_t st;

    switch (op) {
    case FMI_OP_SET_CONFIG:
        st = FMI_STATUS_OK;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: fmi%d opcode %u\n", __func__, bus, op);
        st = FMI_STATUS_UNIMPLEMENTED;
        break;
    }
    trace_s5l8940_iop_fmi_cmd(bus, op, iop_ldl(item + 0x10), iop_ldl(item + 0x14),
                              iop_ldl(item + 0x18), iop_ldl(item + 0x1c), st);
    iop_stl(item + CMD_STATUS, st);
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
        s5l8940_aic_sw_set(s->aic, s->ap_vector);
    }
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
        address_space_write(&address_space_memory, s->fw_base + bss, MEMTXATTRS_UNSPECIFIED,
                            zero, end - bss);
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
    s->running = false;
    s->fw_base = s->fw_config = 0;
    s->said_outside = false;
}

static void s5l8940_iop_realize(DeviceState *dev, Error **errp)
{
    S5L8940IOPState *s = S5L8940_IOP(dev);

    if (!s->aic) {
        error_setg(errp, "s5l8940.iop needs its aic link");
        return;
    }
    qdev_init_gpio_in_named(dev, iop_doorbell, "doorbell", 1);
}

static void s5l8940_iop_init(Object *obj)
{
    S5L8940IOPState *s = S5L8940_IOP(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8940_iop_ops, s, TYPE_S5L8940_IOP, S5L8940_IOP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
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
        VMSTATE_END_OF_LIST()
    }
};

static const Property s5l8940_iop_props[] = {
    DEFINE_PROP_LINK("aic", S5L8940IOPState, aic, TYPE_S5L8940_AIC, DeviceState *),
    DEFINE_PROP_UINT32("ap-vector", S5L8940IOPState, ap_vector, S5L8940_IRQ_IOP),
    DEFINE_PROP_UINT32("window-offset", S5L8940IOPState, window_offset, 0x40000000),
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
    .class_init    = s5l8940_iop_class_init,
};

static void s5l8940_iop_register_types(void)
{
    type_register_static(&s5l8940_iop_info);
}

type_init(s5l8940_iop_register_types)
