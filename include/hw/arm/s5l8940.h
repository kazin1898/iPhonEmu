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
#define S5L8940_USB_PHY_BASE     0x36000000   /* otgphyctrl,s5l8940x */
#define S5L8940_USB_OTG_BASE     0x36100000   /* usb-complex ranges: usb-device, Synopsys DWC OTG */
#define S5L8940_USB_EHCI_BASE    0x36400000   /* usb-ehci reg[0]; reg[1] 0x36700000 stays unimplemented */
#define S5L8940_USB_OHCI0_BASE   0x36500000
#define S5L8940_USB_OHCI1_BASE   0x36600000
#define S5L8940_IOP_BASE         0x3f300000   /* iop,s5l8940x: the AP-side control block (AppleA5IOP) */
#define S5L8940_IOP_SIZE         0x1000

/*
 * Display: the A4 blocks at the A5 addresses (hw/arm/s5l8930_display.c for the
 * pipes, CLCD, RGBOUT and TV-out; hw/arm/ipod_touch_mipi_dsi.c for the DSIM).
 * DT clcd reg[0..2], rgbout reg[0..1], tv-out, mipi-dsim reg[0] (reg[1],
 * 0x3f110000, is the "sac" the kernel never reaches for), dart-nrt, dart-rt.
 */
#define S5L8940_DISP_PIPE0_BASE  0x3a100000   /* AppleH4PDisplayPipe, 0x7000 */
#define S5L8940_CLCD_BASE        0x39200000   /* AppleH4PCLCD's timing generator, 0x2000 */
#define S5L8940_CLCD_AUX_BASE    0x39300000   /* clcd reg[2], 0x1000: new on the A5 */
#define S5L8940_RGBOUT_BASE      0x3a200000   /* AppleH4PRGBOUT's own DisplayPipe */
#define S5L8940_RGBOUT2_BASE     0x39600000   /* its control block */
#define S5L8940_TVOUT_BASE       0x39400000
#define S5L8940_DSIM_BASE        0x39500000   /* mipi-dsim-1,samsung */
#define S5L8940_DART_NRT_BASE    0x38b00000   /* dart-nrt: mapper-jpeg, mapper-scaler */
#define S5L8940_DART_RT_BASE     0x3a400000   /* dart-rt: mapper-isp */
#define S5L8940_IRQ_TVOUT        40
#define S5L8940_IRQ_DSIM         41
#define S5L8940_IRQ_CLCD         42           /* DT clcd interrupts[1] */
#define S5L8940_IRQ_DISP_PIPE0   43           /* DT clcd interrupts[0]: the pipe's frame interrupt */
#define S5L8940_IRQ_RGBOUT_PIPE  44           /* DT rgbout interrupts[0] */
#define S5L8940_IRQ_RGBOUT       45

/* AIC vector numbers (DT interrupts) */
#define S5L8940_IRQ_WDT          4
#define S5L8940_IRQ_UART(n)      (21 + (n))
#define S5L8940_IRQ_GPIO         119
#define S5L8940_IRQ_CDMA(ch)     (50 + (ch))  /* DT cdma interrupts: channels 1..44 at 51..94 */
#define S5L8940_IRQ_I2C(n)       (18 + (n))
#define S5L8940_IRQ_USB_OTG      11
#define S5L8940_IRQ_USB_EHCI     12
#define S5L8940_IRQ_USB_OHCI0    13
#define S5L8940_IRQ_USB_OHCI1    14
#define S5L8940_IRQ_IOP          2            /* IOP -> AP (DT iop interrupts) */
#define S5L8940_IRQ_IOP_DOORBELL 3            /* AP -> IOP: the AIC "IPID" function's argument */
#define S5L8940_IRQ_FMI(n)       (33 + (n))
#define S5L8940_AIC_NUM_IRQS     192          /* AIC_INFO[9:0]; the IPI vectors follow at 0xc0.. */

/*
 * Apple Interrupt Controller (hw/arm/s5l8940_aic.c). One MMIO region of
 * S5L8940_AIC_SIZE. GPIO inputs 0..num-irqs-1 are the hardware interrupt
 * lines (level). Sysbus IRQ S5L8940_AIC_IRQ(n) is CPU n's IRQ and
 * S5L8940_AIC_FIQ(n) its FIQ (the per-CPU timer); the outputs are sized for
 * S5L8940_AIC_MAX_CPUS whatever num-cpus says.
 */
#define TYPE_S5L8940_AIC "s5l8940.aic"
/* Raise a software vector from outside the AP: the HLE IOP's answer line. */
void s5l8940_aic_sw_set(DeviceState *dev, unsigned vector);
/*
 * IOP (hw/arm/s5l8940_iop.c): the A5 IOP's host-visible surface, high-level: MMIO 0 = the
 * control block (S5L8940_IOP_SIZE at S5L8940_IOP_BASE); link "aic"; GPIO in "doorbell" (from
 * the AIC's "doorbell" output). Answers the EmbeddedIOP rings in guest DRAM.
 */
#define TYPE_S5L8940_IOP "s5l8940.iop"
/*
 * AE2, the audio coprocessor (hw/arm/s5l8940_ae2.c), high-level: MMIO 0 control block, 1 mailbox,
 * 2 power/clock block, 3 configuration block, 4 SRAM; sysbus IRQ 0 its interrupt to the AP (DT ae2
 * interrupts[0]). Answers the EmbeddedIOP rings of AppleA5AE2's firmware in guest DRAM.
 */
#define TYPE_S5L8940_AE2 "s5l8940.ae2"
#define S5L8940_AE2_CTRL_BASE    0x341e0000
#define S5L8940_AE2_MBOX_BASE    0x341e1000
#define S5L8940_AE2_PWR_BASE     0x341a0000
#define S5L8940_AE2_CFG_BASE     0x341c1000
#define S5L8940_AE2_SRAM_BASE    0x34000000
#define S5L8940_IRQ_AE2          97

/* I2C controller (hw/arm/s5l8940_i2c.c): MMIO 0 (0x1000), sysbus IRQ 0, child bus "i2c". */
#define TYPE_S5L8940_I2C "s5l8940.i2c"
/* Dialog D1881 PMU on i2c0 at 0x74 (hw/arm/s5l8940_d1881.c): a register file for now. */
#define TYPE_S5L8940_D1881 "s5l8940.d1881"
#define S5L8940_AIC_MAX_CPUS     4
#define S5L8940_AIC_IRQ(c)       (c)
#define S5L8940_AIC_FIQ(c)       (S5L8940_AIC_MAX_CPUS + (c))

#endif
