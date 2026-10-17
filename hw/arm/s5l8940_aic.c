/*
 * Apple Interrupt Controller, the A5's (S5L8940X, DT "aic,1" at 0x3F200000).
 *
 * Register map, as the 10B329 AppleInterruptController kext drives it
 * (docs/n94/hardware.md "AIC") and the Linux apple-aic driver's version-1
 * layout agrees:
 *
 *   0x0004 INFO        [9:0] external vectors (192), [20:16] CPUs - 1
 *   0x0010 GLB_CFG     the kext ORs 0x70 in at start; the timer FIQ is held
 *                      back until then (see below)
 *   0x0020 TIME_LO     free-running counter, low word
 *   0x0028 TIME_HI     high word
 *   0x2000 WHOAMI      index of the CPU reading
 *   0x2004 IACK        event: [18:16] type (0 none, 1 external vector in
 *                      [9:0], 4 IPI with [1:0] 1 = from another CPU, 2 = self)
 *   0x2008 IPI_SET     bit n: IPI to CPU n; bit 31: IPI to self
 *   0x200c IPI_CLR     write 1 to clear the current CPU's pending IPI bits
 *   0x2010 TMR_CFG     per-CPU timer, current CPU
 *   0x2014 TMR_CNT
 *   0x2018 TMR_INT_STAT
 *   0x201c TMR_STATE_SET
 *   0x2020 TMR_STATE_CLR
 *   0x2024 IPI_MASK_SET   bit 0: IPIs from others, bit 31: self
 *   0x2028 IPI_MASK_CLR
 *   0x3000 + 4n  TARGET_CPU[n]   bit c: vector n may interrupt CPU c
 *   0x4000 + 4w  SW_SET      software-triggered vectors, 32 per word
 *   0x4080 + 4w  SW_CLR
 *   0x4100 + 4w  MASK_SET
 *   0x4180 + 4w  MASK_CLR    (reads return the mask)
 *   0x4200 + 4w  HW_STATE    the input lines
 *   0x5000 + 0x80c + 0x10..0x28  CPU c's bank of the 0x2010..0x2028 registers
 *
 * The kext loops on IACK until it reads type 0 and never unmasks a vector
 * after handling it, so acknowledging does not mask here: a level line
 * that stays up is reported again. The kext panics on event type 6
 * ("kAICIackVecTypeTimer"): the per-CPU timer reaches the kernel's rtclock
 * as FIQ, not through IACK. The 10B329 kernel's rtclock writes TMR_CNT
 * 0x7fffffff, TMR_CFG 1 and TMR_INT_STAT 1 at init, then TMR_CNT alone for
 * each deadline and TMR_INT_STAT 1 from the FIQ handler, and reads TIME_HI,
 * TIME_LO, TIME_HI for mach_absolute_time (24 MHz, the DT
 * timebase-frequency). So: TMR_CFG bit 0 enables the timer, TMR_CNT loads a
 * countdown at the timebase rate, reaching zero sets INT_STAT bit 0 and the
 * FIQ, and writing INT_STAT clears both (W1C). TMR_STATE_SET/CLR are kept as
 * bits; the kernel does not use them.
 *
 * The kernel arms its first real deadline (about 9.5 ms) before its FIQ
 * dispatcher has a timer callback (cpu_data + 0x30 is still NULL: the FIQ
 * entry at 0x80087b0c acks TMR_INT_STAT and drops the event), and never
 * re-arms after a dropped one, so a FIQ delivered that early stops the
 * clock for good. On the hardware it evidently is not delivered until
 * AppleInterruptController::start writes GLB_CFG |= 0x70, after the
 * callback is in place: the FIQ output is gated on GLB_CFG_FIQ_EN here, an
 * expired timer staying pending until then. -trace s5l8940_aic* logs every
 * access.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/cpu.h"
#include "hw/arm/s5l8940.h"
#include "migration/vmstate.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(S5L8940AICState, S5L8940_AIC)

#define AIC_MAX_CPUS        S5L8940_AIC_MAX_CPUS
#define AIC_MAX_IRQS        1024
#define AIC_WORDS           (AIC_MAX_IRQS / 32)
#define AIC_TIMEBASE_HZ     24000000

#define AIC_REV             0x0000
#define AIC_INFO            0x0004
#define AIC_GLB_CFG         0x0010
#define AIC_TIME_LO         0x0020
#define AIC_TIME_HI         0x0028
#define AIC_WHOAMI          0x2000
#define AIC_IACK            0x2004
#define AIC_IPI_SET         0x2008
#define AIC_IPI_CLR         0x200c
#define AIC_CPU_BANK        0x2000          /* current CPU's 0x10..0x28 */
#define AIC_CPU_BANK_END    0x202c
#define AIC_TARGET          0x3000
#define AIC_SW_SET          0x4000
#define AIC_SW_CLR          0x4080
#define AIC_MASK_SET        0x4100
#define AIC_MASK_CLR        0x4180
#define AIC_HW_STATE        0x4200
#define AIC_HW_STATE_END    0x4280
#define AIC_PER_CPU         0x5000          /* + 0x80 * cpu */
#define AIC_PER_CPU_STRIDE  0x80
#define AIC_PER_CPU_END     (AIC_PER_CPU + AIC_MAX_CPUS * AIC_PER_CPU_STRIDE)

/* Offsets inside a CPU bank */
#define BANK_TMR_CFG        0x10
#define BANK_TMR_CNT        0x14
#define BANK_TMR_INT_STAT   0x18
#define BANK_TMR_STATE_SET  0x1c
#define BANK_TMR_STATE_CLR  0x20
#define BANK_IPI_MASK_SET   0x24
#define BANK_IPI_MASK_CLR   0x28

#define IACK_TYPE_SHIFT     16
#define IACK_TYPE_NONE      0
#define IACK_TYPE_IRQ       1
#define IACK_TYPE_IPI       4
#define IACK_IPI_OTHER      1
#define IACK_IPI_SELF       2

#define IPI_OTHER           (1u << 0)
#define IPI_SELF            (1u << 31)

#define TMR_CFG_ENABLE      (1u << 0)
#define GLB_CFG_FIQ_EN      0x70            /* the kext's start bits; which of them gates the FIQ is unknown */
#define TMR_INT_FIRED       (1u << 0)

typedef struct S5L8940AICCpu {
    qemu_irq irq, fiq;
    uint32_t ipi_pending;       /* IPI_OTHER | IPI_SELF */
    uint32_t ipi_mask;
    uint32_t tmr_cfg;
    uint32_t tmr_load;          /* TMR_CNT as written */
    uint32_t tmr_state;
    uint32_t tmr_int_stat;
    uint64_t tmr_start;         /* timebase tick the countdown began at */
    QEMUTimer *timer;
    struct S5L8940AICState *aic;
    int index;
} S5L8940AICCpu;

struct S5L8940AICState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t num_cpus;
    uint32_t num_irqs;
    int64_t tick_base_ns;

    uint32_t glb_cfg;
    uint32_t hw_state[AIC_WORDS];
    uint32_t sw_state[AIC_WORDS];
    uint32_t mask[AIC_WORDS];
    uint32_t target[AIC_MAX_IRQS];
    S5L8940AICCpu cpu[AIC_MAX_CPUS];
    /*
     * The IOP's doorbell (hw/arm/s5l8940_iop.c): the iop node's function-doorbell is the
     * AIC platform function "IPID" with argument 3, and the HLE IOP has no CPU to take an
     * interrupt, so the vector the AP raises for it rings this line instead: SW_SET of
     * vector doorbell-vector, or IPI_SET of bit doorbell-ipi (whichever the kext's IPID
     * turns out to write; both are traced).
     */
    qemu_irq doorbell;
    uint32_t doorbell_vector;
    uint32_t doorbell_ipi;
};

static void aic_update(S5L8940AICState *s);

/* A software vector raised from outside the AP (the IOP's answer line, vector 2 on the N94). */
void s5l8940_aic_sw_set(DeviceState *dev, unsigned vector)
{
    S5L8940AICState *s = S5L8940_AIC(dev);

    if (vector < s->num_irqs) {
        s->sw_state[vector / 32] |= 1u << (vector % 32);
        aic_update(s);
    }
}

static uint64_t aic_ticks(S5L8940AICState *s)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->tick_base_ns,
                    AIC_TIMEBASE_HZ, NANOSECONDS_PER_SECOND);
}

static unsigned aic_current_cpu(S5L8940AICState *s)
{
    unsigned c = current_cpu ? current_cpu->cpu_index : 0;

    return c < s->num_cpus ? c : 0;
}

/* The vector IACK would report to CPU c, or -1. Lowest number first. */
static int aic_pending_vector(S5L8940AICState *s, unsigned c)
{
    for (unsigned w = 0; w < DIV_ROUND_UP(s->num_irqs, 32); w++) {
        uint32_t pend = (s->hw_state[w] | s->sw_state[w]) & ~s->mask[w];

        while (pend) {
            unsigned n = w * 32 + ctz32(pend);

            if (n < s->num_irqs && (s->target[n] & (1u << c))) {
                return n;
            }
            pend &= pend - 1;
        }
    }
    return -1;
}

static uint32_t aic_pending_ipi(S5L8940AICCpu *cpu)
{
    return cpu->ipi_pending & ~cpu->ipi_mask;
}

static void aic_update(S5L8940AICState *s)
{
    for (unsigned c = 0; c < s->num_cpus; c++) {
        S5L8940AICCpu *cpu = &s->cpu[c];

        qemu_set_irq(cpu->irq, aic_pending_vector(s, c) >= 0 || aic_pending_ipi(cpu));
        qemu_set_irq(cpu->fiq, (cpu->tmr_int_stat & TMR_INT_FIRED) && (s->glb_cfg & GLB_CFG_FIQ_EN));
    }
}

static uint32_t aic_iack(S5L8940AICState *s, unsigned c)
{
    S5L8940AICCpu *cpu = &s->cpu[c];
    int n = aic_pending_vector(s, c);
    uint32_t ipi;

    if (n >= 0) {
        return (IACK_TYPE_IRQ << IACK_TYPE_SHIFT) | n;
    }
    ipi = aic_pending_ipi(cpu);
    if (ipi & IPI_OTHER) {
        return (IACK_TYPE_IPI << IACK_TYPE_SHIFT) | IACK_IPI_OTHER;
    }
    if (ipi & IPI_SELF) {
        return (IACK_TYPE_IPI << IACK_TYPE_SHIFT) | IACK_IPI_SELF;
    }
    return IACK_TYPE_NONE;
}

static uint32_t tmr_remaining(S5L8940AICCpu *cpu)
{
    uint64_t elapsed = aic_ticks(cpu->aic) - cpu->tmr_start;

    return elapsed < cpu->tmr_load ? cpu->tmr_load - elapsed : 0;
}

static void tmr_arm(S5L8940AICCpu *cpu, uint32_t load)
{
    S5L8940AICState *s = cpu->aic;

    cpu->tmr_start = aic_ticks(s);
    cpu->tmr_load = load;
    if (!(cpu->tmr_cfg & TMR_CFG_ENABLE)) {
        timer_del(cpu->timer);
        return;
    }
    timer_mod(cpu->timer, s->tick_base_ns + 1 +
              muldiv64(cpu->tmr_start + load, NANOSECONDS_PER_SECOND, AIC_TIMEBASE_HZ));
}

/*
 * Expiry raises INT_STAT and reloads the count: the kernel acks some FIQs
 * without writing a new TMR_CNT (its rtclock found nothing earlier to wait
 * for) and would wait forever on a one-shot timer, so the hardware
 * evidently keeps counting from the last value written.
 */
static void tmr_expire(void *opaque)
{
    S5L8940AICCpu *cpu = opaque;

    cpu->tmr_int_stat |= TMR_INT_FIRED;
    trace_s5l8940_aic_timer_fire(cpu->index);
    if (cpu->tmr_load) {
        tmr_arm(cpu, cpu->tmr_load);
    }
    aic_update(cpu->aic);
}

static uint64_t aic_bank_read(S5L8940AICState *s, unsigned c, hwaddr off)
{
    S5L8940AICCpu *cpu = &s->cpu[c];

    switch (off) {
    case BANK_TMR_CFG:
        return cpu->tmr_cfg;
    case BANK_TMR_CNT:
        return (cpu->tmr_cfg & TMR_CFG_ENABLE) ? tmr_remaining(cpu) : cpu->tmr_load;
    case BANK_TMR_INT_STAT:
        return cpu->tmr_int_stat;
    case BANK_TMR_STATE_SET:
    case BANK_TMR_STATE_CLR:
        return cpu->tmr_state;
    case BANK_IPI_MASK_SET:
    case BANK_IPI_MASK_CLR:
        return cpu->ipi_mask;
    }
    qemu_log_mask(LOG_UNIMP, "%s: cpu%u bank read 0x%02x\n", __func__, c, (unsigned)off);
    return 0;
}

static void aic_bank_write(S5L8940AICState *s, unsigned c, hwaddr off, uint32_t val)
{
    S5L8940AICCpu *cpu = &s->cpu[c];

    switch (off) {
    case BANK_TMR_CFG:
        if ((cpu->tmr_cfg ^ val) & TMR_CFG_ENABLE) {
            cpu->tmr_load = tmr_remaining(cpu);
            cpu->tmr_cfg = val;
            tmr_arm(cpu, cpu->tmr_load);
        }
        cpu->tmr_cfg = val;
        return;
    case BANK_TMR_CNT:
        tmr_arm(cpu, val);
        return;
    case BANK_TMR_INT_STAT:
        cpu->tmr_int_stat &= ~val;
        return;
    case BANK_TMR_STATE_SET:
        cpu->tmr_state |= val;
        return;
    case BANK_TMR_STATE_CLR:
        cpu->tmr_state &= ~val;
        return;
    case BANK_IPI_MASK_SET:
        cpu->ipi_mask |= val & (IPI_OTHER | IPI_SELF);
        return;
    case BANK_IPI_MASK_CLR:
        cpu->ipi_mask &= ~val;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "%s: cpu%u bank write 0x%02x <- 0x%08x\n",
                  __func__, c, (unsigned)off, val);
}

static uint64_t s5l8940_aic_read(void *opaque, hwaddr off, unsigned size)
{
    S5L8940AICState *s = opaque;
    unsigned c = aic_current_cpu(s);
    uint32_t val = 0;

    switch (off) {
    case AIC_REV:
        val = 0;
        break;
    case AIC_INFO:
        val = (s->num_irqs & 0x3ff) | ((s->num_cpus - 1) << 16);
        break;
    case AIC_GLB_CFG:
        val = s->glb_cfg;
        break;
    case AIC_TIME_LO:
        val = (uint32_t)aic_ticks(s);
        break;
    case AIC_TIME_HI:
        val = aic_ticks(s) >> 32;
        break;
    case AIC_WHOAMI:
        val = c;
        break;
    case AIC_IACK:
        val = aic_iack(s, c);
        break;
    case AIC_IPI_SET:
    case AIC_IPI_CLR:
        val = s->cpu[c].ipi_pending;
        break;
    case BANK_TMR_CFG + AIC_CPU_BANK ... AIC_CPU_BANK_END - 4:
        val = aic_bank_read(s, c, off - AIC_CPU_BANK);
        break;
    case AIC_TARGET ... AIC_SW_SET - 4:
        val = s->target[(off - AIC_TARGET) / 4];
        break;
    case AIC_SW_SET ... AIC_SW_CLR - 4:
        val = s->sw_state[(off - AIC_SW_SET) / 4];
        break;
    case AIC_SW_CLR ... AIC_MASK_SET - 4:
        val = s->sw_state[(off - AIC_SW_CLR) / 4];
        break;
    case AIC_MASK_SET ... AIC_MASK_CLR - 4:
        val = s->mask[(off - AIC_MASK_SET) / 4];
        break;
    case AIC_MASK_CLR ... AIC_HW_STATE - 4:
        val = s->mask[(off - AIC_MASK_CLR) / 4];
        break;
    case AIC_HW_STATE ... AIC_HW_STATE_END - 4:
        val = s->hw_state[(off - AIC_HW_STATE) / 4];
        break;
    case AIC_PER_CPU ... AIC_PER_CPU_END - 4: {
        unsigned bc = (off - AIC_PER_CPU) / AIC_PER_CPU_STRIDE;
        hwaddr boff = (off - AIC_PER_CPU) % AIC_PER_CPU_STRIDE;

        if (bc < s->num_cpus && boff >= BANK_TMR_CFG && boff <= BANK_IPI_MASK_CLR) {
            val = aic_bank_read(s, bc, boff);
        } else {
            qemu_log_mask(LOG_UNIMP, "%s: unmodeled read 0x%04x\n", __func__, (unsigned)off);
        }
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unmodeled read 0x%04x\n", __func__, (unsigned)off);
        break;
    }
    trace_s5l8940_aic_read(c, (uint32_t)off, val);
    return val;
}

static void s5l8940_aic_write(void *opaque, hwaddr off, uint64_t val64, unsigned size)
{
    S5L8940AICState *s = opaque;
    unsigned c = aic_current_cpu(s);
    uint32_t val = val64;

    trace_s5l8940_aic_write(c, (uint32_t)off, val);
    switch (off) {
    case AIC_GLB_CFG:
        s->glb_cfg = val;
        break;
    case AIC_IPI_SET:
        for (unsigned t = 0; t < s->num_cpus; t++) {
            if (val & (1u << t)) {
                s->cpu[t].ipi_pending |= IPI_OTHER;
            }
        }
        if (val & IPI_SELF) {
            s->cpu[c].ipi_pending |= IPI_SELF;
        }
        if (s->doorbell_ipi < 31 && (val & (1u << s->doorbell_ipi))) {
            trace_s5l8940_aic_doorbell("IPI_SET", val);
            qemu_irq_pulse(s->doorbell);
        }
        break;
    case AIC_IPI_CLR:
        s->cpu[c].ipi_pending &= ~val;
        break;
    case BANK_TMR_CFG + AIC_CPU_BANK ... AIC_CPU_BANK_END - 4:
        aic_bank_write(s, c, off - AIC_CPU_BANK, val);
        break;
    case AIC_TARGET ... AIC_SW_SET - 4:
        s->target[(off - AIC_TARGET) / 4] = val;
        break;
    case AIC_SW_SET ... AIC_SW_CLR - 4:
        s->sw_state[(off - AIC_SW_SET) / 4] |= val;
        if ((off - AIC_SW_SET) / 4 == s->doorbell_vector / 32 && (val & (1u << (s->doorbell_vector % 32)))) {
            trace_s5l8940_aic_doorbell("SW_SET", val);
            qemu_irq_pulse(s->doorbell);
        }
        break;
    case AIC_SW_CLR ... AIC_MASK_SET - 4:
        s->sw_state[(off - AIC_SW_CLR) / 4] &= ~val;
        break;
    case AIC_MASK_SET ... AIC_MASK_CLR - 4:
        s->mask[(off - AIC_MASK_SET) / 4] |= val;
        break;
    case AIC_MASK_CLR ... AIC_HW_STATE - 4:
        s->mask[(off - AIC_MASK_CLR) / 4] &= ~val;
        break;
    case AIC_PER_CPU ... AIC_PER_CPU_END - 4: {
        unsigned bc = (off - AIC_PER_CPU) / AIC_PER_CPU_STRIDE;
        hwaddr boff = (off - AIC_PER_CPU) % AIC_PER_CPU_STRIDE;

        if (bc < s->num_cpus && boff >= BANK_TMR_CFG && boff <= BANK_IPI_MASK_CLR) {
            aic_bank_write(s, bc, boff, val);
        } else {
            qemu_log_mask(LOG_UNIMP, "%s: unmodeled write 0x%04x <- 0x%08x\n",
                          __func__, (unsigned)off, val);
        }
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unmodeled write 0x%04x <- 0x%08x\n",
                      __func__, (unsigned)off, val);
        return;
    }
    aic_update(s);
}

static const MemoryRegionOps s5l8940_aic_ops = {
    .read = s5l8940_aic_read,
    .write = s5l8940_aic_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void s5l8940_aic_set_irq(void *opaque, int n, int level)
{
    S5L8940AICState *s = opaque;
    uint32_t bit = 1u << (n % 32);

    if (level) {
        s->hw_state[n / 32] |= bit;
    } else {
        s->hw_state[n / 32] &= ~bit;
    }
    aic_update(s);
}

static void s5l8940_aic_reset(DeviceState *dev)
{
    S5L8940AICState *s = S5L8940_AIC(dev);

    s->glb_cfg = 0;
    memset(s->sw_state, 0, sizeof(s->sw_state));
    memset(s->mask, 0xff, sizeof(s->mask));          /* everything masked */
    memset(s->target, 0, sizeof(s->target));
    s->tick_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned c = 0; c < AIC_MAX_CPUS; c++) {
        S5L8940AICCpu *cpu = &s->cpu[c];

        timer_del(cpu->timer);
        cpu->ipi_pending = 0;
        cpu->ipi_mask = IPI_OTHER | IPI_SELF;
        cpu->tmr_cfg = cpu->tmr_load = cpu->tmr_state = cpu->tmr_int_stat = 0;
        cpu->tmr_start = 0;
    }
    aic_update(s);
}

static void s5l8940_aic_realize(DeviceState *dev, Error **errp)
{
    S5L8940AICState *s = S5L8940_AIC(dev);

    if (s->num_cpus < 1 || s->num_cpus > AIC_MAX_CPUS) {
        error_setg(errp, "num-cpus must be 1..%d", AIC_MAX_CPUS);
        return;
    }
    if (s->num_irqs < 1 || s->num_irqs > AIC_MAX_IRQS) {
        error_setg(errp, "num-irqs must be 1..%d", AIC_MAX_IRQS);
        return;
    }
    qdev_init_gpio_in(dev, s5l8940_aic_set_irq, s->num_irqs);
    qdev_init_gpio_out_named(dev, &s->doorbell, "doorbell", 1);
}

static void s5l8940_aic_init(Object *obj)
{
    S5L8940AICState *s = S5L8940_AIC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8940_aic_ops, s, TYPE_S5L8940_AIC,
                          S5L8940_AIC_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    for (unsigned c = 0; c < AIC_MAX_CPUS; c++) {
        sysbus_init_irq(sbd, &s->cpu[c].irq);
    }
    for (unsigned c = 0; c < AIC_MAX_CPUS; c++) {
        S5L8940AICCpu *cpu = &s->cpu[c];

        sysbus_init_irq(sbd, &cpu->fiq);
        cpu->aic = s;
        cpu->index = c;
        cpu->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tmr_expire, cpu);
    }
}

static void s5l8940_aic_finalize(Object *obj)
{
    S5L8940AICState *s = S5L8940_AIC(obj);

    for (unsigned c = 0; c < AIC_MAX_CPUS; c++) {
        timer_free(s->cpu[c].timer);
    }
}

static const VMStateDescription vmstate_s5l8940_aic_cpu = {
    .name = "s5l8940.aic.cpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ipi_pending, S5L8940AICCpu),
        VMSTATE_UINT32(ipi_mask, S5L8940AICCpu),
        VMSTATE_UINT32(tmr_cfg, S5L8940AICCpu),
        VMSTATE_UINT32(tmr_load, S5L8940AICCpu),
        VMSTATE_UINT32(tmr_state, S5L8940AICCpu),
        VMSTATE_UINT32(tmr_int_stat, S5L8940AICCpu),
        VMSTATE_UINT64(tmr_start, S5L8940AICCpu),
        VMSTATE_TIMER_PTR(timer, S5L8940AICCpu),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_s5l8940_aic = {
    .name = TYPE_S5L8940_AIC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(tick_base_ns, S5L8940AICState),
        VMSTATE_UINT32(glb_cfg, S5L8940AICState),
        VMSTATE_UINT32_ARRAY(hw_state, S5L8940AICState, AIC_WORDS),
        VMSTATE_UINT32_ARRAY(sw_state, S5L8940AICState, AIC_WORDS),
        VMSTATE_UINT32_ARRAY(mask, S5L8940AICState, AIC_WORDS),
        VMSTATE_UINT32_ARRAY(target, S5L8940AICState, AIC_MAX_IRQS),
        VMSTATE_STRUCT_ARRAY(cpu, S5L8940AICState, AIC_MAX_CPUS, 1,
                             vmstate_s5l8940_aic_cpu, S5L8940AICCpu),
        VMSTATE_END_OF_LIST()
    }
};

static const Property s5l8940_aic_props[] = {
    DEFINE_PROP_UINT32("num-cpus", S5L8940AICState, num_cpus, 2),
    DEFINE_PROP_UINT32("num-irqs", S5L8940AICState, num_irqs, S5L8940_AIC_NUM_IRQS),
    DEFINE_PROP_UINT32("doorbell-vector", S5L8940AICState, doorbell_vector, 3),
    DEFINE_PROP_UINT32("doorbell-ipi", S5L8940AICState, doorbell_ipi, 3),
};

static void s5l8940_aic_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, s5l8940_aic_props);
    dc->realize = s5l8940_aic_realize;
    dc->vmsd = &vmstate_s5l8940_aic;
    device_class_set_legacy_reset(dc, s5l8940_aic_reset);
}

static const TypeInfo s5l8940_aic_info = {
    .name          = TYPE_S5L8940_AIC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8940AICState),
    .instance_init = s5l8940_aic_init,
    .instance_finalize = s5l8940_aic_finalize,
    .class_init    = s5l8940_aic_class_init,
};

static void s5l8940_aic_register_types(void)
{
    type_register_static(&s5l8940_aic_info);
}

type_init(s5l8940_aic_register_types)
