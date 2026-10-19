/*
 * S5L8930 ("A4") PMGR: PLLs, clock-config and clock-gate registers, the
 * 24 MHz timebase and event timers (+0x2000), the watchdog (+0x2020) and
 * POWER_ID (+0x6000). One block because the kernel maps it as one: the DT
 * pmgr node is both device_type "timer" (pe_arm_init_timer) and
 * AppleS5L8930XPerformanceController's register window.
 *
 * Register contract: docs/research/gap-kernel-platform-mmio.md §1.1-1.3.
 * Nothing here decodes clock frequencies; the kernel takes those from the DT.
 * The PLL/clock-config values only have to satisfy the kernel's polls.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/arm/s5l8930.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "system/runstate.h"
#include "trace.h"
#include "hw/trace-printf.h"

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930PMGRState, S5L8930_PMGR)

#define PMGR_TIMEBASE_HZ    24000000

/* PLL CON0 at 0x00/0x08/0x10/0x20 (A/M/E/VPLL), CON1 at +4. */
#define PLL_CON0_ENABLE     (1u << 31)
#define PLL_CON0_LOCKED     (1u << 29)
#define PLL_CON0_UPDATE     (1u << 27)
#define PLL_END             0x28

/* 48 clock-config regs; bit 30 is the "change pending" bit the kernel polls. */
#define CLKCFG_START        0x40
#define CLKCFG_END          0x100
#define CLKCFG_BUSY         (1u << 30)

/* DPSM slots 0x100 + 0x10*i, i = 0..3; the kernel polls these two bits clear. */
#define DPSM_START          0x100
#define DPSM_END            0x140
#define DPSM_BUSY           (0x40000000u | 0x200u)

/* Gate n at 0x1010 + 4n: bits 0-3 request, 4-7 actual, bit 31 reset pulse. The A4 has 0x40 gates;
 * the A5 ("gates" property) reaches 0x1180 and polls gate 0x44 at boot. */
#define GATE_START          0x1010
#define GATE_END(s)         (GATE_START + 4 * (s)->gates)
#define GATE_RESET          (1u << 31)

/* The A5's second bank of clock-config registers ("clkcfg-hi"): the same busy bit. */
#define CLKCFG_HI_START     0x200
#define CLKCFG_HI_END       0x300

#define PMGR_GATE_CTL       0x1200

/*
 * The A5's core control ("cores" property; pmgr,s5l8940x). The DT cpus nodes' enable_core is the
 * PMGR platform function 'Core' with argument 1 << n; the 10B329 PMGR kext starts core n with
 * CORE_START <- mask, CORE_RUN <- mask, then 0x1204 <- 0, and stops it with CORE_STOP <- mask.
 * At start it writes 0x1200 <- 0x7ffe and 0x1204 <- 0x3fff8001. Core n's state goes out on the
 * "core-enable" GPIO line n, for the machine to power the CPU on (at its reset vector) or off.
 */
#define PMGR_CORE_STOP      0x1210
#define PMGR_CORE_START     0x1214
#define PMGR_CORE_RUN       0x1220
#define PMGR_MAX_CORES      2

#define PMGR_TICKS_LO       0x2000
#define PMGR_TICKS_HI       0x2004
#define PMGR_EVT_COUNT(n)   (0x2008 + 4 * (n))
#define PMGR_EVT_STATE(n)   (0x2010 + 4 * (n))
#define EVT_STATE_START     (1u << 0)
#define EVT_STATE_UPDATE    (1u << 1)

/*
 * Watchdog; the names follow iBoot's rPMGR_WDOG_TMR/RST/INTR/CTL. At +0x2020 on the A4, +0x3020 on the
 * A5 (the "a5" property; DT wdt reg 0x3F103020), the same four registers, which the 10B329
 * AppleS5L8930XWatchDogTimer drives as: arm = TMR <- 0, INTR <- RST / 2, RST <- 10 s of ticks, CTL <- 0xd;
 * its interrupt (AIC 4) handler sets CTL bit 1 (acknowledge) and kicks; disarm = CTL <- 0; and the
 * platform function 'WDTr', the kernel's way to restart the machine (a panic's restart among them),
 * CTL |= 4, RST <- 0, TMR <- 0, then spins until the reset.
 */
#define WDOG_TMR            0x0
#define WDOG_RST            0x4
#define WDOG_INTR           0x8
#define WDOG_CTL            0xc
#define WDOG_CTL_IRQ_EN     (1u << 0)
#define WDOG_CTL_IRQ        (1u << 1)       /* raised at INTR; write 1 to clear */
#define WDOG_CTL_RESET_EN   (1u << 2)
#define PMGR_WDOG_A4        0x2020
#define PMGR_WDOG_A5        0x3020
#define WDOG(s, r)          (((s)->a5 ? PMGR_WDOG_A5 : PMGR_WDOG_A4) + (r))

/*
 * The A5's PMGR registers the 10B329 AppleS5L8940XPerformanceController uses beyond the A4's ("a5"):
 * CPU_PERF the CPU's performance state (written from cpu-clk-cfgs, then polled until bit 30 clears),
 * PERF_STATE the SoC's (a byte, polled until [23:16] clear), PWR_REQ the power domains it brings up
 * (bit 16 + n set in its mask, polled until the register shows none of them pending), and
 * configuration words written at start (0x3c, 0x2108 <- 0x51, 0x3100-0x3108 from the DT).
 */
#define A5_CPU_PERF         0x38
#define A5_CPU_PERF_BUSY    (1u << 30)
#define A5_PERF_CFG         0x3c
#define A5_PERF_STATE       0x300
#define A5_PERF_STATE_BUSY  0x00ff0000u
#define A5_PWR_STATE        0x2104
#define A5_PWR_PENDING      0xffff0000u
#define A5_PWR_CFG          0x2108
#define A5_CFG_START        0x3100
#define A5_CFG_END          0x310c

#define PMGR_CLOCK_CON1     0x5030
#define CLOCK_CON1_BUSY     (1u << 16)
#define PMGR_POWER_ID       0x6000

typedef struct S5L8930EventTimer {
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t count;     /* count buffer, as last written */
    uint32_t state;
    uint64_t start;     /* timebase tick the countdown began at */
    uint32_t load;      /* value it counts down from */
    bool pending;
} S5L8930EventTimer;

struct S5L8930PMGRState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t regs[S5L8930_PMGR_SIZE / 4];
    uint8_t security_epoch;     /* POWER_ID[31:24] LLB would latch; 0 = measured */
    uint8_t board_id;           /* POWER_ID[23:16], the board straps */
    uint32_t gates;             /* "gates": clock gates at 0x1010 (A4 0x40, A5 more) */
    bool clkcfg_hi;             /* "clkcfg-hi": 0x200-0x300 are clock configs too (A5) */
    uint32_t cores;             /* "cores": the A5's core control at 0x1200-0x1220 (0: none) */
    uint32_t core_on;           /* bit n: core n running (core 0 from reset) */
    qemu_irq core_enable[PMGR_MAX_CORES];
    int64_t tick_base_ns;
    bool keep_timebase;         /* "keep-timebase": the next reset is the AP's wake, see reset */
    S5L8930EventTimer evt[2];
    QEMUTimer *wdog_timer;
    uint64_t wdog_start;
    qemu_irq wdog_irq;
    bool a5;                    /* "a5": the S5L8940X's watchdog address and registers */
};

/*
 * Reset values stand in for iBoot's state, since the kernel is booted
 * without it. PLLs are iEmu's (APLL 1000 MHz, EPLL 1026 MHz, VPLL 48 MHz;
 * IEMU/s5l8930.c:388-398), except MPLL, which iEmu leaves off: 400 MHz here
 * (M=100 P=3 S=2), since the A4's memory clock comes from somewhere.
 * POWER_ID: epoch 1, board-id 2, read from a real K48AP (iEmu had epoch 2).
 */
static const struct {
    hwaddr off;
    uint32_t val;
} pmgr_defaults[] = {
    { 0x00, 0xA00187D1 }, { 0x04, 0x00380960 },
    { 0x08, 0xA000C322 }, { 0x0C, 0x00380960 },
    { 0x10, 0xA0010559 }, { 0x14, 0x00380960 },
    { 0x20, 0xA0008205 }, { 0x24, 0x00380960 },
    /* Clock muxes/dividers reconstructed from the real K48 clock-frequencies
     * property and iBoot's PMGR decoder. Zero divisors mean an unavailable
     * clock; leaving all registers zero made iBoot publish a zero FMI clock. */
    { 0x40, 0x00802000 },
    { 0x44, 0x00000005 },
    { 0x48, 0x2000000a },
    { 0x4c, 0x20000001 },
    { 0x50, 0x00000006 },
    { 0x54, 0x2000000f },
    { 0x5c, 0x20000013 },
    { 0x60, 0x20000003 },
    { 0x64, 0x00000000 },
    { 0x68, 0x20000015 },
    { 0x70, 0x00000002 },
    { 0x74, 0x10000002 },
    { 0x78, 0x00000005 },
    { 0x80, 0x00000000 },
    { 0x84, 0x2000000f },
    { 0x88, 0x20000003 },
    { 0x8c, 0x20000006 },
    { 0x90, 0x30000001 },
    { 0x94, 0x2000000f },
    { 0x98, 0x00000000 },
    { 0x9c, 0x00000000 },
    { 0xa0, 0x00000000 },
    { 0xa4, 0x10000000 },
    { 0xa8, 0x30000000 },
    { 0xac, 0x10000000 },
    { 0xb0, 0x10000000 },
    { 0xb4, 0x10000000 },
    { 0xb8, 0x00000000 },
    { 0xbc, 0x00000000 },
    { 0xc4, 0x0000000c },
    { 0xc8, 0x00000000 },
    { 0xcc, 0x00000000 },
    { 0xd0, 0x10000000 },
    { 0xd4, 0x00000002 },
    { 0xdc, 0x20000000 },
    { 0xe0, 0x20000000 },
    { 0xe4, 0x20000000 },
    { 0xe8, 0x20000000 },
    { 0xec, 0x20000000 },
    { 0xf0, 0x00000002 },
    { 0xf4, 0x00000002 },
    { 0xf8, 0x00000002 },
    { 0xfc, 0x00000000 },
    { PMGR_POWER_ID, 0x01000001 },   /* measured on a real K48AP (epoch 1); board-id ORed in */
};

static bool pmgr_clkcfg(S5L8930PMGRState *s, hwaddr off)
{
    return (off >= CLKCFG_START && off < CLKCFG_END) ||
           (s->clkcfg_hi && off >= CLKCFG_HI_START && off < CLKCFG_HI_END);
}

static bool pmgr_modeled(S5L8930PMGRState *s, hwaddr off)
{
    return off < PLL_END ||
           (off >= CLKCFG_START && off < DPSM_END) ||
           pmgr_clkcfg(s, off) ||
           (off >= GATE_START && off < GATE_END(s)) ||
           off == PMGR_GATE_CTL ||
           (s->cores && off >= PMGR_GATE_CTL && off <= PMGR_CORE_RUN) ||
           (off >= PMGR_TICKS_LO && off <= PMGR_EVT_STATE(1)) ||
           (off >= WDOG(s, WDOG_TMR) && off <= WDOG(s, WDOG_CTL)) ||
           (s->a5 && (off == A5_CPU_PERF || off == A5_PERF_CFG || off == A5_PERF_STATE ||
                      off == A5_PWR_STATE || off == A5_PWR_CFG ||
                      (off >= A5_CFG_START && off < A5_CFG_END))) ||
           off == PMGR_CLOCK_CON1 || off == PMGR_POWER_ID;
}

static uint64_t pmgr_ticks(S5L8930PMGRState *s)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->tick_base_ns,
                    PMGR_TIMEBASE_HZ, NANOSECONDS_PER_SECOND);
}

static uint32_t evt_remaining(S5L8930PMGRState *s, S5L8930EventTimer *t)
{
    uint64_t elapsed = pmgr_ticks(s) - t->start;

    return elapsed < t->load ? t->load - elapsed : 0;
}

/* (Re)start the countdown from `load` ticks, now. */
static void evt_arm(S5L8930PMGRState *s, S5L8930EventTimer *t, uint32_t load)
{
    t->start = pmgr_ticks(s);
    t->load = load;
    if (!(t->state & EVT_STATE_START)) {
        timer_del(t->timer);
        return;
    }
    /* +1 ns: muldiv64 rounds down, and expiring before the counter gets
     * there would hand rtclock a deadline that has not yet passed. */
    timer_mod(t->timer, s->tick_base_ns + 1 +
              muldiv64(t->start + load, NANOSECONDS_PER_SECOND,
                       PMGR_TIMEBASE_HZ));
}

/* Expiry asserts the line and stops; it stays up until a STATE write with
 * UPDATE, which is how the kernel's FIQ entry acks it (3 then 1). */
static void evt_expire(void *opaque)
{
    S5L8930EventTimer *t = opaque;

    t->pending = true;
    qemu_irq_raise(t->irq);
}

static void evt_write_state(S5L8930PMGRState *s, S5L8930EventTimer *t,
                            uint32_t val)
{
    t->state = val;
    if (val & EVT_STATE_UPDATE) {
        t->pending = false;
        qemu_irq_lower(t->irq);
        evt_arm(s, t, t->count);
    } else if (!(val & EVT_STATE_START)) {
        timer_del(t->timer);
    } else if (!t->pending && !timer_pending(t->timer)) {
        /* START without UPDATE on a stopped timer resumes what was left. */
        evt_arm(s, t, evt_remaining(s, t));
    }
}

/* The watchdog shares the PMGR timebase. Keep the written count latched and
 * add elapsed ticks while it is enabled (interrupt or reset). A comparison already satisfied
 * requests the immediate reset used by WDTr; a future comparison needs a
 * virtual deadline even if the guest never accesses PMGR again. */
static bool wdog_counting(S5L8930PMGRState *s)
{
    return s->regs[WDOG(s, WDOG_CTL) / 4] & (WDOG_CTL_RESET_EN | WDOG_CTL_IRQ_EN);
}

static uint32_t wdog_count(S5L8930PMGRState *s)
{
    uint32_t count = s->regs[WDOG(s, WDOG_TMR) / 4];
    if (wdog_counting(s)) {
        uint64_t elapsed = pmgr_ticks(s) - s->wdog_start;
        return MIN((uint64_t)count + elapsed, UINT32_MAX);
    }
    return count;
}

static void wdog_update_irq(S5L8930PMGRState *s)
{
    uint32_t ctl = s->regs[WDOG(s, WDOG_CTL) / 4];

    qemu_set_irq(s->wdog_irq, (ctl & WDOG_CTL_IRQ) && (ctl & WDOG_CTL_IRQ_EN));
}

static void wdog_check(S5L8930PMGRState *s)
{
    uint32_t *ctl = &s->regs[WDOG(s, WDOG_CTL) / 4];
    uint32_t count = wdog_count(s);
    uint64_t next = UINT64_MAX;

    timer_del(s->wdog_timer);
    if ((*ctl & WDOG_CTL_IRQ_EN) && !(*ctl & WDOG_CTL_IRQ)) {
        uint32_t intr = s->regs[WDOG(s, WDOG_INTR) / 4];

        if (count >= intr) {
            *ctl |= WDOG_CTL_IRQ;
        } else {
            next = intr - count;
        }
    }
    wdog_update_irq(s);
    if (*ctl & WDOG_CTL_RESET_EN) {
        uint32_t limit = s->regs[WDOG(s, WDOG_RST) / 4];

        if (count >= limit) {
            trace_s5l8930_pmgr_wdog_reset(count, limit);
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            return;
        }
        next = MIN(next, limit - count);
    }
    if (next != UINT64_MAX) {
        timer_mod(s->wdog_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1 +
                  muldiv64(next, NANOSECONDS_PER_SECOND, PMGR_TIMEBASE_HZ));
    }
}

static void wdog_expire(void *opaque)
{
    wdog_check(opaque);
}

static uint64_t s5l8930_pmgr_read(void *opaque, hwaddr off, unsigned size)
{
    S5L8930PMGRState *s = opaque;
    uint32_t val = s->regs[off / 4];

    if (!pmgr_modeled(s, off)) {
        qemu_log_mask(LOG_UNIMP, "%s: unmodeled read 0x%04x\n",
                      __func__, (unsigned)off);
        return val;
    }

    if (off == WDOG(s, WDOG_TMR)) {
        return wdog_count(s);
    }
    if (s->a5) {
        switch (off) {
        case A5_CPU_PERF:
            return val & ~A5_CPU_PERF_BUSY;
        case A5_PERF_STATE:
            return val & ~A5_PERF_STATE_BUSY;
        case A5_PWR_STATE:
            return val & ~A5_PWR_PENDING;
        }
    }

    switch (off) {
    /* The kernel reads HI, LO, HI and retries on a mismatch, so both halves
     * can be sampled live. */
    case PMGR_TICKS_LO:
        return (uint32_t)pmgr_ticks(s);
    case PMGR_TICKS_HI:
        return pmgr_ticks(s) >> 32;
    case PMGR_EVT_COUNT(0):
    case PMGR_EVT_COUNT(1): {
        S5L8930EventTimer *t = &s->evt[(off - PMGR_EVT_COUNT(0)) / 4];
        return (t->state & EVT_STATE_START) ? evt_remaining(s, t) : t->count;
    }
    case PMGR_EVT_STATE(0):
    case PMGR_EVT_STATE(1):
        return s->evt[(off - PMGR_EVT_STATE(0)) / 4].state;
    case PMGR_CLOCK_CON1:
        return val & ~CLOCK_CON1_BUSY;
    }

    if (off < PLL_END && !(off & 4)) {
        return (val & PLL_CON0_ENABLE) ? val | PLL_CON0_LOCKED
                                       : val & ~PLL_CON0_LOCKED;
    }
    if (pmgr_clkcfg(s, off)) {
        return val & ~CLKCFG_BUSY;
    }
    if (off >= DPSM_START && off < DPSM_END) {
        return val & ~DPSM_BUSY;
    }
    return val;
}

static void s5l8930_pmgr_write(void *opaque, hwaddr off, uint64_t val64,
                               unsigned size)
{
    S5L8930PMGRState *s = opaque;
    uint32_t val = val64;

    if (!pmgr_modeled(s, off)) {
        qemu_log_mask(LOG_UNIMP, "%s: unmodeled write 0x%04x <- 0x%08x\n",
                      __func__, (unsigned)off, val);
        s->regs[off / 4] = val;
        return;
    }

    switch (off) {
    case PMGR_TICKS_LO:
    case PMGR_TICKS_HI:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to timebase 0x%04x\n",
                      __func__, (unsigned)off);
        return;
    case PMGR_EVT_COUNT(0):
    case PMGR_EVT_COUNT(1): {
        /* rtclock's set_decrementer writes only this while running, so it
         * has to restart the countdown by itself. */
        S5L8930EventTimer *t = &s->evt[(off - PMGR_EVT_COUNT(0)) / 4];
        t->count = val;
        if (!t->pending) {
            evt_arm(s, t, val);
        }
        return;
    }
    case PMGR_EVT_STATE(0):
    case PMGR_EVT_STATE(1):
        evt_write_state(s, &s->evt[(off - PMGR_EVT_STATE(0)) / 4], val);
        return;
    case PMGR_CORE_STOP:
    case PMGR_CORE_START:
        if (s->cores) {
            uint32_t mask = val & MAKE_64BIT_MASK(0, s->cores);

            s->core_on = off == PMGR_CORE_START ? s->core_on | mask : s->core_on & ~mask;
            for (unsigned n = 0; n < s->cores; n++) {
                if (mask & (1u << n)) {
                    trace_s5l8930_pmgr_core(n, off == PMGR_CORE_START);
                    qemu_set_irq(s->core_enable[n], off == PMGR_CORE_START);
                }
            }
        }
        s->regs[off / 4] = val;
        return;
    }
    if (off >= WDOG(s, WDOG_TMR) && off <= WDOG(s, WDOG_CTL)) {
        trace_s5l8930_pmgr_wdog(off - WDOG(s, WDOG_TMR), val);
        s->regs[WDOG(s, WDOG_TMR) / 4] = wdog_count(s);
        s->wdog_start = pmgr_ticks(s);
        if (off == WDOG(s, WDOG_CTL)) {
            /* bit 1 is the interrupt's status: writing 1 clears it */
            uint32_t irq = s->regs[off / 4] & WDOG_CTL_IRQ & ~val;

            val = (val & ~WDOG_CTL_IRQ) | irq;
        }
        s->regs[off / 4] = val;
        wdog_check(s);
        return;
    }

    if (trace_event_get_state_backends(TRACE_S5L8930_PMGR_LOG) && off < GATE_START) {
        TRACE_PRINTF(trace_s5l8930_pmgr_log, "[PMGR] W %04x <- %08x\n", (unsigned)off, val);
    }
    if (off < PLL_END && !(off & 4)) {
        val &= ~PLL_CON0_UPDATE;
    } else if (off >= GATE_START && off < GATE_END(s)) {
        /* Settle at once: actual (4-7) follows request (0-3), and the
         * reset pulse is over as soon as it is written. */
        val = (val & ~(GATE_RESET | 0xF0)) | ((val & 0xF) << 4);
    }
    s->regs[off / 4] = val;
}

static const MemoryRegionOps s5l8930_pmgr_ops = {
    .read = s5l8930_pmgr_read,
    .write = s5l8930_pmgr_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void s5l8930_pmgr_reset(DeviceState *dev)
{
    S5L8930PMGRState *s = S5L8930_PMGR(dev);
    int i;

    memset(s->regs, 0, sizeof(s->regs));
    for (i = 0; i < ARRAY_SIZE(pmgr_defaults); i++) {
        s->regs[pmgr_defaults[i].off / 4] = pmgr_defaults[i].val;
    }
    s->regs[PMGR_POWER_ID / 4] |= (uint32_t)s->board_id << 16;
    /* iboot= skips LLB, which writes the boot epoch here on hardware; the
     * machine hands over what that LLB would write (it_iboot_find_miu_epoch). */
    if (s->security_epoch) {
        s->regs[PMGR_POWER_ID / 4] = (s->regs[PMGR_POWER_ID / 4] & 0x00ffffff) |
                                     (uint32_t)s->security_epoch << 24;
    }
    /* Every clock source enabled and every gate on: the kernel builds its
     * enabled masks from these, and enabling a power gate panics unless its
     * clock gate already reads 0xF. */
    for (i = CLKCFG_START; i < CLKCFG_END; i += 4) {
        s->regs[i / 4] |= 0x80000000;
    }
    if (s->clkcfg_hi) {
        for (i = CLKCFG_HI_START; i < CLKCFG_HI_END; i += 4) {
            s->regs[i / 4] |= 0x80000000;
        }
    }
    for (i = GATE_START; i < GATE_END(s); i += 4) {
        s->regs[i / 4] = 0xFF;
    }

    timer_del(s->wdog_timer);
    s->wdog_start = 0;
    qemu_irq_lower(s->wdog_irq);
    s->core_on = 1;             /* the machine's reset leaves the other cores off */
    /*
     * The 24 MHz timebase keeps counting across the AP's hibernate: xnu's
     * mach_absolute_time must not go back, or every timer armed before the sleep
     * (launchd's intervals, configd's ThermalMonitor) waits until the counter
     * catches up with the uptime at sleep. The machine sets "keep-timebase"
     * before the wake's reset (kboot boards; no LLB restores it).
     */
    if (!s->keep_timebase) {
        s->tick_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    s->keep_timebase = false;
    for (i = 0; i < ARRAY_SIZE(s->evt); i++) {
        S5L8930EventTimer *t = &s->evt[i];

        timer_del(t->timer);
        qemu_irq_lower(t->irq);
        t->count = t->state = t->load = 0;
        t->start = 0;
        t->pending = false;
    }
}

static bool pmgr_get_keep_timebase(Object *obj, Error **errp)
{
    return (((S5L8930PMGRState *)obj))->keep_timebase;
}

static void pmgr_set_keep_timebase(Object *obj, bool value, Error **errp)
{
    ((S5L8930PMGRState *)obj)->keep_timebase = value;
}

static void s5l8930_pmgr_init(Object *obj)
{
    object_property_add_bool(obj, "keep-timebase", pmgr_get_keep_timebase, pmgr_set_keep_timebase);
    S5L8930PMGRState *s = S5L8930_PMGR(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    int i;

    s->wdog_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdog_expire, s);
    memory_region_init_io(&s->iomem, obj, &s5l8930_pmgr_ops, s,
                          TYPE_S5L8930_PMGR, S5L8930_PMGR_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_out_named(DEVICE(obj), s->core_enable, "core-enable", PMGR_MAX_CORES);
    for (i = 0; i < ARRAY_SIZE(s->evt); i++) {
        sysbus_init_irq(sbd, &s->evt[i].irq);
    }
    sysbus_init_irq(sbd, &s->wdog_irq);          /* sysbus IRQ 2: the watchdog's interrupt */
    for (i = 0; i < ARRAY_SIZE(s->evt); i++) {
        s->evt[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, evt_expire,
                                       &s->evt[i]);
    }
}

/* All times are QEMU_CLOCK_VIRTUAL, which migration carries, so the counter
 * and pending deadlines resume where they were. The IRQ line level lives
 * in the VIC's state. */
static const VMStateDescription vmstate_s5l8930_evt = {
    .name = "s5l8930.pmgr.evt",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, S5L8930EventTimer),
        VMSTATE_UINT32(count, S5L8930EventTimer),
        VMSTATE_UINT32(state, S5L8930EventTimer),
        VMSTATE_UINT64(start, S5L8930EventTimer),
        VMSTATE_UINT32(load, S5L8930EventTimer),
        VMSTATE_BOOL(pending, S5L8930EventTimer),
        VMSTATE_END_OF_LIST()
    }
};

static int s5l8930_pmgr_post_load(void *opaque, int version_id)
{
    S5L8930PMGRState *s = opaque;
    if (version_id < 2) {
        s->wdog_start = pmgr_ticks(s);
        wdog_check(s);
    }
    return 0;
}

static void s5l8930_pmgr_finalize(Object *obj)
{
    S5L8930PMGRState *s = S5L8930_PMGR(obj);
    timer_free(s->wdog_timer);
    for (int i = 0; i < ARRAY_SIZE(s->evt); i++) {
        timer_free(s->evt[i].timer);
    }
}

static const VMStateDescription vmstate_s5l8930_pmgr = {
    .name = TYPE_S5L8930_PMGR,
    .version_id = 3,
    .minimum_version_id = 1,
    .post_load = s5l8930_pmgr_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8930PMGRState, S5L8930_PMGR_SIZE / 4),
        VMSTATE_INT64(tick_base_ns, S5L8930PMGRState),
        VMSTATE_TIMER_PTR_V(wdog_timer, S5L8930PMGRState, 2),
        VMSTATE_UINT64_V(wdog_start, S5L8930PMGRState, 2),
        VMSTATE_UINT32_V(core_on, S5L8930PMGRState, 3),
        VMSTATE_STRUCT_ARRAY(evt, S5L8930PMGRState, 2, 1,
                             vmstate_s5l8930_evt, S5L8930EventTimer),
        VMSTATE_END_OF_LIST()
    }
};

static const Property s5l8930_pmgr_props[] = {
    DEFINE_PROP_UINT8("security-epoch", S5L8930PMGRState, security_epoch, 0),
    DEFINE_PROP_UINT8("board-id", S5L8930PMGRState, board_id, 0x02),   /* K48 */
    DEFINE_PROP_UINT32("gates", S5L8930PMGRState, gates, 0x40),
    DEFINE_PROP_BOOL("clkcfg-hi", S5L8930PMGRState, clkcfg_hi, false),
    DEFINE_PROP_UINT32("cores", S5L8930PMGRState, cores, 0),
    DEFINE_PROP_BOOL("a5", S5L8930PMGRState, a5, false),
};

static void s5l8930_pmgr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, s5l8930_pmgr_props);

    dc->vmsd = &vmstate_s5l8930_pmgr;
    device_class_set_legacy_reset(dc, s5l8930_pmgr_reset);
}

static const TypeInfo s5l8930_pmgr_info = {
    .name          = TYPE_S5L8930_PMGR,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930PMGRState),
    .instance_init = s5l8930_pmgr_init,
    .instance_finalize = s5l8930_pmgr_finalize,
    .class_init    = s5l8930_pmgr_class_init,
};

static void s5l8930_pmgr_register_types(void)
{
    type_register_static(&s5l8930_pmgr_info);
}

type_init(s5l8930_pmgr_register_types)
