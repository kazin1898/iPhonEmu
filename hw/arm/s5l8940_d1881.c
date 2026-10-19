/*
 * Dialog D1881 PMU, the iPhone 4S's, on i2c0 at 0x74 (DT pmu,d1881). A
 * register file with the parts AppleD1881PMU (10B329, text 0x80aea268) is
 * seen to depend on:
 *
 *   0x01-0x07  EVENT_A..G, read by the interrupt handler (7 bytes at once)
 *              and cleared by that read; EVENT_B bit 0x20 is "ADC done".
 *   0x08-0x0d  STATUS_A..F: a USB host's cable (VBUS) and charging, see d1881_reset.
 *   0x0e-0x14  IRQ_MASK_A..G, 1 = masked (start writes ff 5f ff ff ...).
 *   0x43       ADC control: channel in [4:0], 0x20 starts a conversion (the
 *              driver also sets 0x40/0x80 for some channels), cleared after.
 *   0x44-0x45  ADC result: [3:0] of 0x44 are the low nibble, 0x45 the high
 *              byte of a 12-bit value (_readADCGated: (lo & 0xf) | hi << 4).
 *   0x5d-0x60  the RTC counter, 32-bit little-endian seconds, read twice
 *              until stable (FUN_80aec388); 0xa4-0xa7 is the offset the RTC
 *              class adds to it (setCurrentDateTime writes it).
 *   0x96       system control: 0x2b (after 0xa1 <- 0) is the kernel's
 *              "pmu restart"; the part resets the SoC, and so does this.
 *   0x15       bit 0 is standby: the kernel's halt ("pmu go stdby", 10B329
 *              0x80aed7a0) sets it last and waits for the power to go. The
 *              machine powers off (QEMU's guest shutdown).
 *   0xaf       written by that same halt (0x10, or 0x90) when a cable powers
 *              the phone: it restarts instead, and iBoot shows the battery
 *              charging with iOS off until the power button. There is no
 *              iBoot here, so a restart that follows that write powers the
 *              machine off as well.
 *
 * The buttons: the part sees the menu (Home) and hold (power) buttons too, and a press sets EVENT_A
 * bit 0 or 1 (gpio-in "button" 0 and 1, wired by the machine next to the GPIO pins). Awake, the
 * kernel keeps both masked (IRQ_MASK_A ends at 0xc7) and only reads them with the other events; they
 * are the wake reason the DT's wake_button_menu / wake_button_hold report: the PMU function 'STAT'
 * 0x180 / 0x181 (AppleD1881PMU 0x80aee18c: argument bit 7 = the events latched at the last interrupt,
 * register (arg & 0x3f) >> 3, bit arg & 7, else STATUS_A + that register; dock acc_detect is 'STAT' 5,
 * STATUS_A bit 5).
 *
 * The interrupt line is the DT's GPIO 0x0d (active low); the model's
 * gpio-out 0 is 1 while an unmasked event is pending. Everything else holds
 * what was written and reads it back; -trace s5l8940_d1881* shows what the
 * kernel asks for.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/arm/s5l8940.h"
#include "migration/vmstate.h"
#include "system/runstate.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(S5L8940D1881State, S5L8940_D1881)

#define D1881_EVENT_A       0x01
#define D1881_EVENT_COUNT   7
#define D1881_IRQ_MASK_A    0x0e
#define D1881_EVENT_B_ADC   0x20
#define D1881_EVENT_A_VBUS  0x08        /* PMU interrupt 3: the power source re-reads the status */
#define D1881_EVENT_A_MENU  0x01        /* the Home button pressed ('STAT' 0x180) */
#define D1881_EVENT_A_HOLD  0x02        /* the power button pressed ('STAT' 0x181) */
#define D1881_EVENT_A_USB   0x40
#define D1881_STATUS_A      0x08        /* STATUS_A..F, read 6 at once */
#define D1881_STATUS_A_VBUS     0x08
#define D1881_STATUS_A_VBUS_RE  0x40    /* VBUS as a re-attach reads it */
#define D1881_STATUS_B      0x09
#define D1881_STATUS_B_CHARGING 0x02
#define D1881_ADC_CTRL      0x43
#define D1881_ADC_START     0x20
#define D1881_ADC_RES_L     0x44
#define D1881_ADC_RES_H     0x45
#define D1881_RTC_COUNT     0x5d
#define D1881_SYS_CONTROL   0x96
#define D1881_SYS_RESTART   0x2b
#define D1881_STANDBY       0x15
#define D1881_STANDBY_GO    0x01
#define D1881_HALT_REASON   0xaf
#define D1881_HALT_ON_CABLE 0x10

struct S5L8940D1881State {
    I2CSlave parent_obj;
    qemu_irq irq;
    uint8_t regs[256];
    uint8_t ptr;
    bool ptr_set;           /* the first byte of a write names the register */
    uint32_t rtc_latch;     /* the counter as of the read of its first byte */
    uint32_t vbat_mv;       /* "vbat-mv": the battery's voltage, ADC muxes 0 and 4 */
};

static void d1881_update_irq(S5L8940D1881State *s)
{
    bool pending = false;

    for (int i = 0; i < D1881_EVENT_COUNT; i++) {
        pending |= (s->regs[D1881_EVENT_A + i] & ~s->regs[D1881_IRQ_MASK_A + i]) != 0;
    }
    qemu_set_irq(s->irq, pending);
}

/*
 * A 12-bit reading per ADC mux (AppleD1881PMU's conversions, FUN_80aec9b0, the same scalings as the
 * A4 boards' D1815): a phone at room temperature on a USB host's cable.
 *   0, 4   vcc_main, vbat: mV = 2500 + raw * 2000 / 4096, from the "vbat-mv" property (default
 *          4037, settable at run time). The power source reads vbat at boot and looks it up in its
 *          battery-data curve (family 3: 4037 mV is ~80%), and that estimate is the level SpringBoard
 *          shows (mid-scale, 3500 mV, was ~2%, red); the machine derives vbat-mv from its battery-level.
 *   6      the dock's D+/D- (otgphyctrl dmon_voltage): 0 V, a host's pull-downs; both lines above
 *          1 V would make it a charger brick.
 *   2, 7, 11-14  NTCs, 10 kOhm at 25 C behind a 50 uA source: 0x333.
 *   8      tjunc, 26 C + 0.32 C * (0x425 - raw): 0x418 is 30 C.
 *   15     vbus, V = raw * 88 / 65536: 5.0 V.
 * Every other mux reads mid-scale.
 */
static uint16_t d1881_adc_sample(S5L8940D1881State *s, unsigned chan)
{
    switch (chan) {
    case 0:
    case 4:
        return MIN((MAX(s->vbat_mv, 2500) - 2500) * 4096 / 2000, 0xfff);
    case 6:
        return 0;
    case 2: case 7: case 11: case 12: case 13: case 14:
        return 0x333;
    case 8:
        return 0x418;
    case 15:
        return 0xe8c;
    default:
        return 0x800;
    }
}

static void d1881_adc_start(S5L8940D1881State *s, uint8_t ctrl)
{
    uint16_t v = d1881_adc_sample(s, ctrl & 0x1f);

    s->regs[D1881_ADC_RES_L] = v & 0xf;
    s->regs[D1881_ADC_RES_H] = v >> 4;
    s->regs[D1881_EVENT_A + 1] |= D1881_EVENT_B_ADC;
    d1881_update_irq(s);
}

static int d1881_event(I2CSlave *i2c, enum i2c_event event)
{
    S5L8940D1881State *s = S5L8940_D1881(i2c);

    if (event == I2C_START_SEND) {
        s->ptr_set = false;
    }
    return 0;
}

static int d1881_send(I2CSlave *i2c, uint8_t data)
{
    S5L8940D1881State *s = S5L8940_D1881(i2c);

    if (!s->ptr_set) {
        s->ptr = data;
        s->ptr_set = true;
        return 0;
    }
    trace_s5l8940_d1881_write(s->ptr, data);
    s->regs[s->ptr] = data;
    if (s->ptr == D1881_SYS_CONTROL && data == D1881_SYS_RESTART) {
        if (s->regs[D1881_HALT_REASON] & D1881_HALT_ON_CABLE) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        } else {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
    } else if (s->ptr == D1881_STANDBY && (data & D1881_STANDBY_GO)) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    } else if (s->ptr == D1881_ADC_CTRL && (data & D1881_ADC_START)) {
        d1881_adc_start(s, data);
    } else if (s->ptr >= D1881_IRQ_MASK_A && s->ptr < D1881_IRQ_MASK_A + D1881_EVENT_COUNT) {
        d1881_update_irq(s);
    }
    s->ptr++;
    return 0;
}

static uint8_t d1881_recv(I2CSlave *i2c)
{
    S5L8940D1881State *s = S5L8940_D1881(i2c);
    uint8_t v;

    if (s->ptr == D1881_RTC_COUNT) {
        s->rtc_latch = qemu_clock_get_ns(QEMU_CLOCK_HOST) / NANOSECONDS_PER_SECOND;
    }
    if (s->ptr >= D1881_RTC_COUNT && s->ptr < D1881_RTC_COUNT + 4) {
        v = s->rtc_latch >> (8 * (s->ptr - D1881_RTC_COUNT));
    } else {
        v = s->regs[s->ptr];
    }
    trace_s5l8940_d1881_read(s->ptr, v);
    if (s->ptr >= D1881_EVENT_A && s->ptr < D1881_EVENT_A + D1881_EVENT_COUNT) {
        s->regs[s->ptr] = 0;            /* events clear on read */
        d1881_update_irq(s);
    }
    s->ptr++;
    return v;
}

static void d1881_reset(DeviceState *dev)
{
    S5L8940D1881State *s = S5L8940_D1881(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->regs + D1881_IRQ_MASK_A, 0xff, D1881_EVENT_COUNT);
    /*
     * A USB host's cable on the dock connector, as a phone left on a computer. The power source
     * (FUN_80aeff38) takes VBUS from STATUS_A bit 3 (bit 6 on a re-attach) and publishes it as
     * AppleUSBCableDetect; with VBUS present and no charger-sleep-interval it keeps idle sleep off
     * ("disabling idle sleep"), as on hardware. That matters here: the AP's hibernate ("pmu go hib")
     * is not modeled, and a guest that slept after Auto-Lock never came back. STATUS_B 0x02 is
     * charging. The cable's event (EVENT_A bit 3, and bit 6 the wake decoder calls "usb") is pending
     * from power-on; the driver evaluates the port at start anyway.
     */
    s->regs[D1881_STATUS_A] = D1881_STATUS_A_VBUS | D1881_STATUS_A_VBUS_RE;
    s->regs[D1881_STATUS_B] = D1881_STATUS_B_CHARGING;
    s->regs[D1881_EVENT_A] = D1881_EVENT_A_VBUS | D1881_EVENT_A_USB;
    s->ptr = 0;
    s->ptr_set = false;
    s->rtc_latch = 0;
    qemu_irq_lower(s->irq);
}

static void d1881_button(void *opaque, int n, int level)
{
    S5L8940D1881State *s = opaque;

    if (level) {
        s->regs[D1881_EVENT_A] |= n ? D1881_EVENT_A_HOLD : D1881_EVENT_A_MENU;
        d1881_update_irq(s);
    }
}

static void d1881_init(Object *obj)
{
    S5L8940D1881State *s = S5L8940_D1881(obj);

    qdev_init_gpio_out(DEVICE(obj), &s->irq, 1);
    qdev_init_gpio_in_named(DEVICE(obj), d1881_button, "button", 2);
    s->vbat_mv = 4037;
    object_property_add_uint32_ptr(obj, "vbat-mv", &s->vbat_mv, OBJ_PROP_FLAG_READWRITE);
}

static const VMStateDescription vmstate_d1881 = {
    .name = TYPE_S5L8940_D1881,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, S5L8940D1881State),
        VMSTATE_UINT8_ARRAY(regs, S5L8940D1881State, 256),
        VMSTATE_UINT8(ptr, S5L8940D1881State),
        VMSTATE_BOOL(ptr_set, S5L8940D1881State),
        VMSTATE_UINT32(rtc_latch, S5L8940D1881State),
        VMSTATE_END_OF_LIST()
    }
};

static void d1881_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = d1881_event;
    k->send = d1881_send;
    k->recv = d1881_recv;
    dc->vmsd = &vmstate_d1881;
    device_class_set_legacy_reset(dc, d1881_reset);
}

static const TypeInfo d1881_info = {
    .name          = TYPE_S5L8940_D1881,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(S5L8940D1881State),
    .instance_init = d1881_init,
    .class_init    = d1881_class_init,
};

static void d1881_register_types(void)
{
    type_register_static(&d1881_info);
}

type_init(d1881_register_types)
