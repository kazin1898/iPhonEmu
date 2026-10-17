/*
 * Dialog D1881 PMU, the iPhone 4S's, on i2c0 at 0x74 (DT pmu,d1881): a
 * register file so AppleD1881PMU's reads are acknowledged. The driver read
 * register 0x5d four bytes at a time in a tight loop for as long as the
 * address NAKed (there was nothing on the bus), which is where the boot
 * stopped. Nothing is modeled yet beyond "a register holds what was written
 * and reads back"; -trace s5l8940_d1881* shows what the kernel asks for, so
 * the registers the drivers depend on (events, RTC, ADC, buttons) can be
 * given meaning one at a time.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/arm/s5l8940.h"
#include "migration/vmstate.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(S5L8940D1881State, S5L8940_D1881)

struct S5L8940D1881State {
    I2CSlave parent_obj;
    uint8_t regs[256];
    uint8_t ptr;
    bool ptr_set;           /* the first byte of a write names the register */
};

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
    s->regs[s->ptr++] = data;
    return 0;
}

static uint8_t d1881_recv(I2CSlave *i2c)
{
    S5L8940D1881State *s = S5L8940_D1881(i2c);
    uint8_t v = s->regs[s->ptr];

    trace_s5l8940_d1881_read(s->ptr, v);
    s->ptr++;
    return v;
}

static void d1881_reset(DeviceState *dev)
{
    S5L8940D1881State *s = S5L8940_D1881(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->ptr = 0;
    s->ptr_set = false;
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
    .class_init    = d1881_class_init,
};

static void d1881_register_types(void)
{
    type_register_static(&d1881_info);
}

type_init(d1881_register_types)
