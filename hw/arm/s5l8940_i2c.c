/*
 * S5L8940X ("A5") I2C controller: the FIFO block AppleS5L8940XI2CController
 * drives (DT "i2c,s5l8940x"), a PA Semi style master like the one Linux's
 * i2c-pasemi serves on later Apple SoCs. Decoded from the 10B329 kext (text
 * at 0x807a5960; its accessors are virtuals +0x370 read / +0x374 write, so
 * every register access is an immediate offset in the code):
 *
 *   0x00 MTXFIFO  write: byte | 0x100 START (the byte is addr << 1 | R/W)
 *                        | 0x200 STOP | 0x400 READ (the byte is a count)
 *   0x04 MRXFIFO  read: next received byte, 0x100 when empty
 *   0x08 FIFO     [7:0] bytes still in the TX FIFO, [15:8] bytes in the RX FIFO
 *                 (the kext only uses it to see progress for its timeout)
 *   0x0c XFER     transfer state (printed in the panic; reads 0 here)
 *   0x14 SMSTA    status, write 1 to clear: 0x10000000 XIP transfer in
 *                 progress, 0x08000000 XEN transfer done, 0x00800000 and
 *                 0x40 error conditions, 0x00200000 MTN address or data NAK,
 *                 0x00100000 receive FIFO not empty (live, not latched: the
 *                 kext never acks it, it drains the FIFO)
 *   0x18 IMASK    interrupt enable (writes: 0x08800040 for a write, 0x00b00040
 *                 for a read, 0 to quiet)
 *   0x1c CTL      clock divider in [7:0] and more; 0x200 is written after a
 *                 NAK (a transmitter reset)
 *   0x2c RXLEN    the read's byte count << 8, written before a read
 *
 * A transaction: SMSTA <- 0x08800040; MTXFIFO <- 0x100 | addr << 1; the
 * bytes to write (the last with 0x200 unless a read follows); for a read
 * MTXFIFO <- 0x101 | addr << 1 then 0x600 | count; then IMASK, and the
 * interrupt handler reads SMSTA, drains MRXFIFO on a read and acks SMSTA.
 * Here the bus transaction completes inside the MTXFIFO write that ends it
 * (STOP, or the READ word), so the status is already up when IMASK is
 * written and the line rises then.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/s5l8940.h"
#include "migration/vmstate.h"

OBJECT_DECLARE_SIMPLE_TYPE(S5L8940I2CState, S5L8940_I2C)

#define REG_MTXFIFO     0x00
#define REG_MRXFIFO     0x04
#define REG_FIFO        0x08
#define REG_XFER        0x0c
#define REG_SMSTA       0x14
#define REG_IMASK       0x18
#define REG_CTL         0x1c
#define REG_RXLEN       0x2c

#define MTXFIFO_START   0x100
#define MTXFIFO_STOP    0x200
#define MTXFIFO_READ    0x400
#define MRXFIFO_EMPTY   0x100

#define SMSTA_XIP       0x10000000
#define SMSTA_XEN       0x08000000
#define SMSTA_MTN       0x00200000
#define SMSTA_RXDONE    0x00100000

#define RX_DEPTH        0x100

struct S5L8940I2CState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;

    uint32_t smsta, imask, ctl, rxlen;
    bool active;            /* a START was written and the bus is held */
    uint8_t rx[RX_DEPTH];
    uint32_t rx_count, rx_pos;
};

static uint32_t smsta_now(S5L8940I2CState *s)
{
    return s->smsta | (s->rx_pos < s->rx_count ? SMSTA_RXDONE : 0);
}

static void i2c_update(S5L8940I2CState *s)
{
    qemu_set_irq(s->irq, (smsta_now(s) & s->imask) != 0);
}

static void i2c_stop(S5L8940I2CState *s)
{
    if (s->active) {
        i2c_end_transfer(s->bus);
        s->active = false;
    }
    s->smsta |= SMSTA_XEN;
}

static void i2c_mtxfifo(S5L8940I2CState *s, uint32_t w)
{
    if (w & MTXFIFO_START) {
        uint8_t addr = (w >> 1) & 0x7f;
        bool recv = w & 1;

        if (s->active) {
            i2c_end_transfer(s->bus);     /* a repeated START: the read after a write */
        }
        s->active = true;
        if (i2c_start_transfer(s->bus, addr, recv)) {
            /* nobody there: the kext sees MTN with XEN and resets the master */
            s->smsta |= SMSTA_MTN;
            i2c_stop(s);
        }
    } else if (w & MTXFIFO_READ) {
        uint32_t n = w & 0xff;

        s->rx_count = s->rx_pos = 0;
        if (s->active) {
            for (uint32_t i = 0; i < n && s->rx_count < RX_DEPTH; i++) {
                s->rx[s->rx_count++] = i2c_recv(s->bus);
            }
        }
        if (w & MTXFIFO_STOP) {
            i2c_stop(s);
        }
        return;
    } else if (s->active) {
        if (i2c_send(s->bus, w & 0xff)) {
            s->smsta |= SMSTA_MTN;
        }
    }
    if (w & MTXFIFO_STOP) {
        i2c_stop(s);
    }
}

static uint64_t s5l8940_i2c_read(void *opaque, hwaddr off, unsigned size)
{
    S5L8940I2CState *s = opaque;

    switch (off) {
    case REG_MRXFIFO:
        if (s->rx_pos < s->rx_count) {
            uint8_t b = s->rx[s->rx_pos++];

            i2c_update(s);
            return b;
        }
        return MRXFIFO_EMPTY;
    case REG_FIFO:
        return (s->rx_count - s->rx_pos) << 8;
    case REG_XFER:
        return 0;
    case REG_SMSTA:
        return smsta_now(s);
    case REG_IMASK:
        return s->imask;
    case REG_CTL:
        return s->ctl;
    case REG_RXLEN:
        return s->rxlen;
    }
    qemu_log_mask(LOG_UNIMP, "%s: unmodeled read 0x%04x\n", __func__, (unsigned)off);
    return 0;
}

static void s5l8940_i2c_write(void *opaque, hwaddr off, uint64_t val64, unsigned size)
{
    S5L8940I2CState *s = opaque;
    uint32_t val = val64;

    switch (off) {
    case REG_MTXFIFO:
        i2c_mtxfifo(s, val);
        break;
    case REG_SMSTA:
        s->smsta &= ~val;
        break;
    case REG_IMASK:
        s->imask = val;
        break;
    case REG_CTL:
        s->ctl = val & ~0x200;      /* the transmitter reset is over at once */
        break;
    case REG_RXLEN:
        s->rxlen = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unmodeled write 0x%04x <- 0x%08x\n",
                      __func__, (unsigned)off, val);
        return;
    }
    i2c_update(s);
}

static const MemoryRegionOps s5l8940_i2c_ops = {
    .read = s5l8940_i2c_read,
    .write = s5l8940_i2c_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void s5l8940_i2c_reset(DeviceState *dev)
{
    S5L8940I2CState *s = S5L8940_I2C(dev);

    s->smsta = s->imask = s->ctl = s->rxlen = 0;
    s->active = false;
    s->rx_count = s->rx_pos = 0;
    qemu_irq_lower(s->irq);
}

static void s5l8940_i2c_init(Object *obj)
{
    S5L8940I2CState *s = S5L8940_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8940_i2c_ops, s, TYPE_S5L8940_I2C, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
}

static const VMStateDescription vmstate_s5l8940_i2c = {
    .name = TYPE_S5L8940_I2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(smsta, S5L8940I2CState),
        VMSTATE_UINT32(imask, S5L8940I2CState),
        VMSTATE_UINT32(ctl, S5L8940I2CState),
        VMSTATE_UINT32(rxlen, S5L8940I2CState),
        VMSTATE_BOOL(active, S5L8940I2CState),
        VMSTATE_UINT8_ARRAY(rx, S5L8940I2CState, RX_DEPTH),
        VMSTATE_UINT32(rx_count, S5L8940I2CState),
        VMSTATE_UINT32(rx_pos, S5L8940I2CState),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8940_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_s5l8940_i2c;
    device_class_set_legacy_reset(dc, s5l8940_i2c_reset);
}

static const TypeInfo s5l8940_i2c_info = {
    .name          = TYPE_S5L8940_I2C,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8940I2CState),
    .instance_init = s5l8940_i2c_init,
    .class_init    = s5l8940_i2c_class_init,
};

static void s5l8940_i2c_register_types(void)
{
    type_register_static(&s5l8940_i2c_info);
}

type_init(s5l8940_i2c_register_types)
