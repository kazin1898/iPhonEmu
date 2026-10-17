/*
 * Apple S5L8940X ("A5") SoC, as used by the iPhone 4S (N94AP) machine.
 *
 * Addresses: 10B329 N94AP device tree (arm-io maps child offsets at
 * 0x30000000; docs/n94/iomap.tsv has every block). The A5 keeps the A4's
 * peripheral offsets: an A4 block at 0x8xxxxxxx sits at 0x3xxxxxxx here and
 * 0xBFxxxxxx at 0x3Fxxxxxx, so the s5l8930_* models serve both.
 */
#ifndef HW_ARM_S5L8940_H
#define HW_ARM_S5L8940_H

#include "hw/core/sysbus.h"

/* Memory */
#define S5L8940_DRAM_BASE        0x80000000
#define S5L8940_DRAM_SIZE        0x20000000   /* 512 MiB on the N94 */

/* Peripheral window: arm-io ranges {0, 0x30000000, 0x10000000} */
#define S5L8940_PERIPH_BASE      0x30000000
#define S5L8940_PERIPH_SIZE      0x10000000

#define S5L8940_UART_BASE(n)     (0x32500000 + (n) * 0x100000)
#define S5L8940_UART_SIZE        0x1000
#define S5L8940_PL310_BASE       0x3e000000   /* L2C-310, DT pl310 reg[0] */
#define S5L8940_PL310_AUX_BASE   0x3fd00000   /* DT pl310 reg[1], 0x2000 */
#define S5L8940_PMGR_BASE        0x3f100000   /* pmgr,s5l8940x: 0x7000; the wdt sits at +0x3020 */
#define S5L8940_PMGR_SIZE        0x8000
#define S5L8940_AIC_BASE         0x3f200000   /* aic,1 */
#define S5L8940_AIC_SIZE         0x10000
#define S5L8940_GPIO_BASE        0x3fa00000   /* gpio,s5l8940x: 28 ports, 8 interrupt groups */
#define S5L8940_GPIO_SIZE        0x1000
#define S5L8940_GPIO_PORTS       0x1c
#define S5L8940_GPIO_INT_GROUPS  8
#define S5L8940_CDMA_BASE        0x37000000   /* cdma-version 3, max-channel 44; the A4 block's layout */
#define S5L8940_AES_BASE         0x37800000   /* its AES filter contexts */
#define S5L8940_I2S_BASE(n)      (0x34190000 + (n) * 0x1000)
#define S5L8940_I2C_BASE(n)      (0x33200000 + (n) * 0x100000)   /* i2c0 PMU/codec, i2c1 compass/ALS, i2c2 accel/gyro */

/* AIC vector numbers (DT interrupts) */
#define S5L8940_IRQ_WDT          4
#define S5L8940_IRQ_UART(n)      (21 + (n))
#define S5L8940_IRQ_GPIO         119
#define S5L8940_IRQ_CDMA(ch)     (50 + (ch))  /* DT cdma interrupts: channels 1..44 at 51..94 */
#define S5L8940_IRQ_I2C(n)       (18 + (n))
#define S5L8940_AIC_NUM_IRQS     192          /* AIC_INFO[9:0]; the IPI vectors follow at 0xc0.. */

/*
 * Apple Interrupt Controller (hw/arm/s5l8940_aic.c). One MMIO region of
 * S5L8940_AIC_SIZE. GPIO inputs 0..num-irqs-1 are the hardware interrupt
 * lines (level). Sysbus IRQ S5L8940_AIC_IRQ(n) is CPU n's IRQ and
 * S5L8940_AIC_FIQ(n) its FIQ (the per-CPU timer); the outputs are sized for
 * S5L8940_AIC_MAX_CPUS whatever num-cpus says.
 */
#define TYPE_S5L8940_AIC "s5l8940.aic"

/* I2C controller (hw/arm/s5l8940_i2c.c): MMIO 0 (0x1000), sysbus IRQ 0, child bus "i2c". */
#define TYPE_S5L8940_I2C "s5l8940.i2c"
/* Dialog D1881 PMU on i2c0 at 0x74 (hw/arm/s5l8940_d1881.c): a register file for now. */
#define TYPE_S5L8940_D1881 "s5l8940.d1881"
#define S5L8940_AIC_MAX_CPUS     4
#define S5L8940_AIC_IRQ(c)       (c)
#define S5L8940_AIC_FIQ(c)       (S5L8940_AIC_MAX_CPUS + (c))

#endif
