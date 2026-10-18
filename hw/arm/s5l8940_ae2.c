/*
 * S5L8940X ("A5") AE2: the audio coprocessor, high-level, in the shape of
 * hw/arm/s5l8940_iop.c. The firmware the kernel hands it
 * (IOP_s5l8940xae2_firmware: "EmbeddedIOP firmware s5l8940xae2-RELEASE",
 * iBoot-1537.9.55, its own task kernel with the "iop", "console", "iop
 * message" and "audio" tasks and the AAC codecs) never executes; this device
 * answers what AppleA5AE2-43.1, EmbeddedIOP and the audio kexts can observe.
 *
 * What the 10B329 AppleA5AE2 does (docs/n94/hardware.md "The AE2"):
 *
 * - start maps the ae2 node's five windows: 0x341E0000 the core's control
 *   block, 0x341E1000 its mailbox, 0x341A0000 a power/clock block,
 *   0x341C1000 a configuration block, 0x34000000 the 192 KiB SRAM (which it
 *   saves and restores around sleep), and registers interrupt 0 (AIC 97).
 * - startCPU: control +4 <- 0, power +0 <- the clock-dividers word, then
 *   polls power +0 bit 16 (busy) clear; control +8 <- the firmware's physical
 *   base (the image runs in place in the firmware kext); configuration +0x14
 *   <- 0x03ff01ff and +0x34 <- 0x01000102 (ae2-version 1); control +4 <-
 *   0x003f0003 (run); mailbox +8 <- 0x10, then 0x20; the interrupt enabled.
 *   Stop writes control +4 <- 0.
 * - The doorbell: mailbox +0x20 <- 0x10 (0x20 for the second source). The
 *   interrupt handler writes mailbox +0x24 <- 0x10 first, then lets
 *   EmbeddedIOP walk its rings.
 *
 * The rings are EmbeddedIOP's, as on the IOP: the firmware's 'cnfg' block
 * (found by its magic: 0x50080 in this image) holds {address, count} per
 * endpoint, 64-byte entries whose word 0 is the item | ownership bit (pending
 * while bit 0 is clear, answered with bit 0 set in place). Control messages
 * ('nop ', 'slep', ...) are answered as the IOP's; every other endpoint's
 * messages are traced (s5l8940_ae2_msg) and answered with their status word
 * untouched until the protocol each kext speaks is modeled.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/s5l8940.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(S5L8940AE2State, S5L8940_AE2)

#define AE2_NREGS           (0x1000 / 4)

#define CTRL_RUN            0x04        /* bit 0: the core runs (0x003f0003) */
#define CTRL_FW_BASE        0x08
#define MBOX_ENABLE         0x08        /* written 0x10, 0x20 at start */
#define MBOX_DOORBELL       0x20        /* AP -> AE2 */
#define MBOX_ACK            0x24        /* W1C of the AE2 -> AP interrupt */
#define MBOX_IRQ_MSG        0x10
#define PWR_CTRL            0x00
#define PWR_BUSY            0x10000

#define FW_CONFIG_MAGIC     0x636e6667  /* 'cnfg' */
#define FW_HDR_BSS_START    0x318
#define FW_HDR_BSS_END      0x31c
#define FW_MAX_SIZE         0x100000
#define FW_CFG_RINGS        0x10
#define FW_CFG_RING(h)      (FW_CFG_RINGS + 8 * (h))
#define FW_CFG_COUNT(h)     (FW_CFG_RINGS + 4 + 8 * (h))
#define AE2_MAX_ENDPOINTS   8
#define AE2_MAX_RING        1024
#define RING_ENTRY_SIZE     64
#define RING_OWNER_AP       1
#define RING_ITEM(w0)       ((w0) & ~3u)
#define RING_CONTROL        0
#define RING_MESSAGE        1           /* AE2 -> AP only */

#define CTRL_OP_SLEP        0x736c6570  /* 'slep' */
#define CTRL_OP_NOP         0x6e6f7020  /* 'nop ' */
#define CTRL_OP_RSUM        0x7273756d  /* 'rsum' */
#define CTRL_OP_SPND        0x73706e64  /* 'spnd' */
#define CTRL_OP_TTIN        0x7474696e  /* 'ttin' */

#define AE2_SRAM_SIZE       0x30000

struct S5L8940AE2State {
    SysBusDevice parent_obj;
    MemoryRegion ctrl_mr, mbox_mr, pwr_mr, cfg_mr, sram;
    qemu_irq irq;
    uint32_t window_offset;     /* AE2 address - AP physical address of DRAM */

    uint32_t ctrl[AE2_NREGS], mbox[AE2_NREGS], pwr[AE2_NREGS], cfg[AE2_NREGS];
    bool running;
    uint32_t fw_base;
    uint32_t fw_config;         /* 'cnfg' physical address, 0 until found */
    uint32_t ring_rx[AE2_MAX_ENDPOINTS];
    uint32_t pending;           /* AE2 -> AP interrupt sources (MBOX_IRQ_MSG) */
    bool said_outside;
    QEMUTimer *irq_timer;
};

static inline uint32_t ae2_ldl(hwaddr addr)
{
    return address_space_ldl_le(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, NULL);
}

static inline void ae2_stl(hwaddr addr, uint32_t val)
{
    address_space_stl_le(&address_space_memory, addr, val, MEMTXATTRS_UNSPECIFIED, NULL);
}

/* An AE2 address as the AP sees it: in the DRAM window, or already physical. */
static hwaddr ae2_pa(S5L8940AE2State *s, uint32_t a)
{
    uint32_t lo = S5L8940_DRAM_BASE + s->window_offset;

    if (s->window_offset && a >= lo && a - lo < S5L8940_DRAM_SIZE) {
        return a - s->window_offset;
    }
    if (!(a >= S5L8940_DRAM_BASE && a - S5L8940_DRAM_BASE < S5L8940_DRAM_SIZE) && !s->said_outside) {
        s->said_outside = true;
        qemu_log_mask(LOG_GUEST_ERROR, "%s: AE2 address 0x%08x outside DRAM\n", __func__, a);
    }
    return a;
}

static void ae2_update_irq(S5L8940AE2State *s)
{
    qemu_set_irq(s->irq, s->pending != 0);
}

/* ---- rings ---------------------------------------------------------------- */

static void ae2_control_message(S5L8940AE2State *s, hwaddr item)
{
    uint32_t op = ae2_ldl(item);
    uint32_t st = 0;

    switch (op) {
    case CTRL_OP_NOP:
    case CTRL_OP_RSUM:
    case CTRL_OP_SPND:
    case CTRL_OP_TTIN:
        break;
    case CTRL_OP_SLEP:
        s->running = false;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: unrecognised host opcode 0x%08x\n", __func__, op);
        st = 1;
        break;
    }
    trace_s5l8940_ae2_ctrl_msg(op, st);
    ae2_stl(item + 4, st);
}

/* A message on an endpoint whose protocol is not modeled yet: traced, handed back. */
static void ae2_endpoint_message(S5L8940AE2State *s, int h, hwaddr item)
{
    uint32_t w[8];

    for (int i = 0; i < 8; i++) {
        w[i] = ae2_ldl(item + 4 * i);
    }
    trace_s5l8940_ae2_msg(h, (uint32_t)item, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
}

static hwaddr ae2_config(S5L8940AE2State *s)
{
    if (!s->fw_config) {
        for (uint32_t off = 0; off + 4 <= FW_MAX_SIZE; off += 4) {
            if (ae2_ldl(s->fw_base + off) == FW_CONFIG_MAGIC) {
                s->fw_config = s->fw_base + off;
                break;
            }
        }
    }
    return s->fw_config;
}

static int ae2_walk_ring(S5L8940AE2State *s, int h, hwaddr ring, uint32_t n)
{
    int served = 0;

    for (uint32_t i = 0; i < n; i++) {
        hwaddr slot = ring + s->ring_rx[h] * RING_ENTRY_SIZE;
        uint32_t w0 = ae2_ldl(slot);
        hwaddr item;

        if ((w0 & 1) == RING_OWNER_AP || RING_ITEM(w0) == 0) {
            break;
        }
        item = ae2_pa(s, RING_ITEM(w0));
        if (h == RING_CONTROL) {
            ae2_control_message(s, item);
        } else {
            ae2_endpoint_message(s, h, item);
        }
        ae2_stl(slot, RING_ITEM(w0) | RING_OWNER_AP);
        s->ring_rx[h] = (s->ring_rx[h] + 1) % n;
        served++;
    }
    return served;
}

static void ae2_doorbell(S5L8940AE2State *s)
{
    hwaddr cfg;
    int served = 0;

    if (!s->running) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: doorbell while stopped\n", __func__);
        return;
    }
    cfg = ae2_config(s);
    if (!cfg) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: no 'cnfg' block in the firmware at 0x%08x\n", __func__, s->fw_base);
        return;
    }
    for (int h = 0; h < AE2_MAX_ENDPOINTS; h++) {
        uint32_t ring = ae2_ldl(cfg + FW_CFG_RING(h));
        uint32_t count = ae2_ldl(cfg + FW_CFG_COUNT(h));

        if (h == RING_MESSAGE || !ring || count == 0) {
            continue;
        }
        if (count > AE2_MAX_RING) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: ring %d has %u entries\n", __func__, h, count);
            continue;
        }
        trace_s5l8940_ae2_ring(h, ring, count);
        served += ae2_walk_ring(s, h, ae2_pa(s, ring), count);
    }
    trace_s5l8940_ae2_doorbell(served);
    if (served) {
        /* A round trip later, as the IOP: an answer inside the doorbell's own store came too early. */
        timer_mod(s->irq_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100 * SCALE_US);
    }
}

static void ae2_irq_expire(void *opaque)
{
    S5L8940AE2State *s = opaque;

    s->pending |= MBOX_IRQ_MSG;
    ae2_update_irq(s);
}

static void ae2_run(S5L8940AE2State *s)
{
    uint32_t bss, end;

    s->fw_base = s->ctrl[CTRL_FW_BASE / 4];
    s->fw_config = 0;
    s->said_outside = false;
    memset(s->ring_rx, 0, sizeof(s->ring_rx));
    if (s->fw_base < S5L8940_DRAM_BASE || s->fw_base >= S5L8940_DRAM_BASE + S5L8940_DRAM_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: firmware base 0x%08x outside DRAM\n", __func__, s->fw_base);
        return;
    }
    bss = ae2_ldl(s->fw_base + FW_HDR_BSS_START);
    end = ae2_ldl(s->fw_base + FW_HDR_BSS_END);
    if (bss < end && end <= FW_MAX_SIZE) {
        g_autofree uint8_t *zero = g_malloc0(end - bss);
        address_space_write(&address_space_memory, s->fw_base + bss, MEMTXATTRS_UNSPECIFIED, zero, end - bss);
    }
    s->running = true;
    trace_s5l8940_ae2_run(s->fw_base, (uint32_t)ae2_config(s));
}

/* ---- registers ------------------------------------------------------------ */

static uint64_t ae2_ctrl_read(void *opaque, hwaddr off, unsigned size)
{
    return ((S5L8940AE2State *)opaque)->ctrl[off / 4];
}

static void ae2_ctrl_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    S5L8940AE2State *s = opaque;

    trace_s5l8940_ae2_reg_write("ctrl", (uint32_t)off, val);
    s->ctrl[off / 4] = val;
    if (off == CTRL_RUN) {
        if (val & 1) {
            ae2_run(s);
        } else {
            s->running = false;
        }
    }
}

static uint64_t ae2_mbox_read(void *opaque, hwaddr off, unsigned size)
{
    S5L8940AE2State *s = opaque;

    if (off == MBOX_ACK) {
        return s->pending;
    }
    return s->mbox[off / 4];
}

static void ae2_mbox_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    S5L8940AE2State *s = opaque;

    trace_s5l8940_ae2_reg_write("mbox", (uint32_t)off, val);
    switch (off) {
    case MBOX_DOORBELL:
        if (val & MBOX_IRQ_MSG) {
            ae2_doorbell(s);
        }
        if (val & ~MBOX_IRQ_MSG) {
            qemu_log_mask(LOG_UNIMP, "%s: doorbell 0x%02x\n", __func__, (uint32_t)val);
        }
        break;
    case MBOX_ACK:
        s->pending &= ~val;
        ae2_update_irq(s);
        break;
    default:
        s->mbox[off / 4] = val;
    }
}

static uint64_t ae2_pwr_read(void *opaque, hwaddr off, unsigned size)
{
    S5L8940AE2State *s = opaque;

    return off == PWR_CTRL ? s->pwr[0] & ~PWR_BUSY : s->pwr[off / 4];
}

static void ae2_pwr_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    S5L8940AE2State *s = opaque;

    trace_s5l8940_ae2_reg_write("pwr", (uint32_t)off, val);
    s->pwr[off / 4] = val;
}

static uint64_t ae2_cfg_read(void *opaque, hwaddr off, unsigned size)
{
    return ((S5L8940AE2State *)opaque)->cfg[off / 4];
}

static void ae2_cfg_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    S5L8940AE2State *s = opaque;

    trace_s5l8940_ae2_reg_write("cfg", (uint32_t)off, val);
    s->cfg[off / 4] = val;
}

#define AE2_OPS(name) \
    static const MemoryRegionOps ae2_##name##_ops = { \
        .read = ae2_##name##_read, \
        .write = ae2_##name##_write, \
        .endianness = DEVICE_NATIVE_ENDIAN, \
        .impl.min_access_size = 4, \
        .impl.max_access_size = 4, \
    }
AE2_OPS(ctrl);
AE2_OPS(mbox);
AE2_OPS(pwr);
AE2_OPS(cfg);

/* ---- device --------------------------------------------------------------- */

static void s5l8940_ae2_reset(DeviceState *dev)
{
    S5L8940AE2State *s = S5L8940_AE2(dev);

    memset(s->ctrl, 0, sizeof(s->ctrl));
    memset(s->mbox, 0, sizeof(s->mbox));
    memset(s->pwr, 0, sizeof(s->pwr));
    memset(s->cfg, 0, sizeof(s->cfg));
    memset(s->ring_rx, 0, sizeof(s->ring_rx));
    s->running = false;
    s->fw_base = s->fw_config = 0;
    s->pending = 0;
    s->said_outside = false;
    timer_del(s->irq_timer);
    ae2_update_irq(s);
}

static void s5l8940_ae2_init(Object *obj)
{
    S5L8940AE2State *s = S5L8940_AE2(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->ctrl_mr, obj, &ae2_ctrl_ops, s, "s5l8940.ae2.ctrl", 0x1000);
    memory_region_init_io(&s->mbox_mr, obj, &ae2_mbox_ops, s, "s5l8940.ae2.mbox", 0x1000);
    memory_region_init_io(&s->pwr_mr, obj, &ae2_pwr_ops, s, "s5l8940.ae2.pwr", 0x1000);
    memory_region_init_io(&s->cfg_mr, obj, &ae2_cfg_ops, s, "s5l8940.ae2.cfg", 0x1000);
    memory_region_init_ram(&s->sram, obj, "s5l8940.ae2.sram", AE2_SRAM_SIZE, &error_fatal);
    sysbus_init_mmio(sbd, &s->ctrl_mr);
    sysbus_init_mmio(sbd, &s->mbox_mr);
    sysbus_init_mmio(sbd, &s->pwr_mr);
    sysbus_init_mmio(sbd, &s->cfg_mr);
    sysbus_init_mmio(sbd, &s->sram);
    sysbus_init_irq(sbd, &s->irq);
    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ae2_irq_expire, s);
}

static void s5l8940_ae2_finalize(Object *obj)
{
    timer_free(S5L8940_AE2(obj)->irq_timer);
}

static const VMStateDescription vmstate_s5l8940_ae2 = {
    .name = TYPE_S5L8940_AE2,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(ctrl, S5L8940AE2State, AE2_NREGS),
        VMSTATE_UINT32_ARRAY(mbox, S5L8940AE2State, AE2_NREGS),
        VMSTATE_UINT32_ARRAY(pwr, S5L8940AE2State, AE2_NREGS),
        VMSTATE_UINT32_ARRAY(cfg, S5L8940AE2State, AE2_NREGS),
        VMSTATE_BOOL(running, S5L8940AE2State),
        VMSTATE_UINT32(fw_base, S5L8940AE2State),
        VMSTATE_UINT32(fw_config, S5L8940AE2State),
        VMSTATE_UINT32_ARRAY(ring_rx, S5L8940AE2State, AE2_MAX_ENDPOINTS),
        VMSTATE_UINT32(pending, S5L8940AE2State),
        VMSTATE_TIMER_PTR(irq_timer, S5L8940AE2State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property s5l8940_ae2_props[] = {
    DEFINE_PROP_UINT32("window-offset", S5L8940AE2State, window_offset, 0x40000000),
};

static void s5l8940_ae2_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, s5l8940_ae2_props);
    dc->vmsd = &vmstate_s5l8940_ae2;
    device_class_set_legacy_reset(dc, s5l8940_ae2_reset);
}

static const TypeInfo s5l8940_ae2_info = {
    .name          = TYPE_S5L8940_AE2,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8940AE2State),
    .instance_init = s5l8940_ae2_init,
    .instance_finalize = s5l8940_ae2_finalize,
    .class_init    = s5l8940_ae2_class_init,
};

static void s5l8940_ae2_register_types(void)
{
    type_register_static(&s5l8940_ae2_info);
}

type_init(s5l8940_ae2_register_types)
