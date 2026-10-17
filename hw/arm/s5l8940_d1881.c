/*
 * Dialog D1881 PMU, the iPhone 4S's, on i2c0 at 0x74 (DT pmu,d1881). A
 * register file with the parts AppleD1881PMU (10B329, text 0x80aea268) is
 * seen to depend on:
 *
 *   0x01-0x07  EVENT_A..G, read by the interrupt handler (7 bytes at once)
 *              and cleared by that read; EVENT_B bit 0x20 is "ADC done".
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
#define D1881_ADC_CTRL      0x43
#define D1881_ADC_START     0x20
#define D1881_ADC_RES_L     0x44
#define D1881_ADC_RES_H     0x45
#define D1881_RTC_COUNT     0x5d
#define D1881_SYS_CONTROL   0x96
#define D1881_SYS_RESTART   0x2b

struct S5L8940D1881State {
    I2CSlave parent_obj;
    qemu_irq irq;
    uint8_t regs[256];
    uint8_t ptr;
    bool ptr_set;           /* the first byte of a write names the register */
    uint32_t rtc_latch;     /* the counter as of the read of its first byte */
};

static void d1881_update_irq(S5L8940D1881State *s)
{
    bool pending = false;

    for (int i = 0; i < D1881_EVENT_COUNT; i++) {
        pending |= (s->regs[D1881_EVENT_A + i] & ~s->regs[D1881_IRQ_MASK_A + i]) != 0;
    }
    qemu_set_irq(s->irq, pending);
}

/* A 12-bit reading per channel: mid-scale until the channels are identified. */
static uint16_t d1881_adc_sample(unsigned chan)
{
    return 0x800;
}

static void d1881_adc_start(S5L8940D1881State *s, uint8_t ctrl)
{
    uint16_t v = d1881_adc_sample(ctrl & 0x1f);

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
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
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
    s->ptr = 0;
    s->ptr_set = false;
    s->rtc_latch = 0;
    qemu_irq_lower(s->irq);
}

static void d1881_init(Object *obj)
{
    qdev_init_gpio_out(DEVICE(obj), &S5L8940_D1881(obj)->irq, 1);
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
