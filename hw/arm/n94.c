/*
 * Apple S5L8940X "A5" board, Cortex-A9: iPhone 4S (N94AP, -M iPhone-4S).
 *
 * Milestone 1 of docs/n94/README.md: enter the iOS 6.1.3 (10B329) kernel
 * directly, with no boot ROM or iBoot, and get its console on UART0. Only
 * what the kernel's platform expert needs is modeled: DRAM, the AIC, the
 * PMGR timers, the GPIO block, the L2 controller and the UARTs. Every other
 * peripheral address falls into an unimplemented-device window that logs the
 * access (-d unimp) instead of faulting, so bring-up can see what the kernel
 * reaches for next.
 *
 * Boot input is a K48KBOOT bundle from tools/ios-prep (the format
 * hw/arm/ipad1.c takes for the A4 boards): a flat image of physical memory
 * (kernel, filled device tree, boot_args) followed by a 24-byte trailer
 * {char magic[8]; u32 load_pa, entry_pa, bootargs_pa, image_len}. It is
 * copied into DRAM on every reset and the CPU starts at entry_pa in ARM
 * state with the MMU off and r0 = bootargs_pa, as iBoot leaves it.
 */

#include "qemu/osdep.h"
#include "hw/arm/machines-qom.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "hw/arm/exynos4210.h"
#include "hw/arm/s5l8930.h"
#include "hw/arm/s5l8940.h"
#include "hw/arm/ipod_touch_cs42l58.h"
#include "hw/arm/ipod_touch_lis302dl.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/ipod_touch_usb_otg.h"
#include "hw/arm/ipod_touch_usb_phys.h"
#include "hw/arm/ipod_touch_mipi_dsi.h"
#include "hw/usb/hcd-ehci.h"
#include "hw/usb/hcd-ohci.h"
#include "system/reset.h"
#include "system/system.h"
#include "target/arm/cpu.h"

#define TYPE_N94_MACHINE MACHINE_TYPE_NAME("iPhone-4S")
OBJECT_DECLARE_SIMPLE_TYPE(N94MachineState, N94_MACHINE)

struct N94MachineState {
    MachineState parent;
    ARMCPU *cpu;
    MemoryRegion dram;
    MemoryRegion dram_lo;                /* DRAM's first page at PA 0 (the kernel's reset-vector page) */
    DeviceState *aic;
    DeviceState *pmgr;
    DeviceState *gpio;
    DeviceState *display;                /* the display pipe, CLCD, RGBOUT, TV-out (hw/arm/s5l8930_display.c) */
    synopsys_usb_state *usb_otg;
    char *kboot_path;
    char *nand_path;                     /* nand=: the PPN page store directory (tools/ios-prep nand) */
};

#define KBOOT_MAGIC "K48KBOOT"
#define KBOOT_TRAILER_LEN 24
#define KBOOT_SEGMENT_LEN 20

static qemu_irq n94_irq(N94MachineState *s, int irq)
{
    return qdev_get_gpio_in(s->aic, irq);
}

/*
 * Stage the kboot bundle and point the CPU at it. Done on every reset, as
 * ipad1.c does: iOS writes over the memory the bundle occupies, so a reset
 * that did not re-stage it would resume into junk.
 */
static void n94_cpu_reset(void *opaque)
{
    N94MachineState *s = N94_MACHINE(opaque);
    CPUState *cs = CPU(s->cpu);
    g_autofree char *data = NULL;
    g_autoptr(GError) gerr = NULL;
    gsize size;
    const uint8_t *trailer;
    uint32_t load_pa, entry_pa, bootargs_pa, image_len;

    cpu_reset(cs);

    if (!g_file_get_contents(s->kboot_path, &data, &size, &gerr)) {
        error_report("n94: cannot read kboot bundle '%s': %s", s->kboot_path, gerr->message);
        exit(1);
    }
    if (size < KBOOT_TRAILER_LEN ||
        memcmp(data + size - KBOOT_TRAILER_LEN, KBOOT_MAGIC, 8) != 0) {
        error_report("n94: '%s' is not a K48KBOOT bundle", s->kboot_path);
        exit(1);
    }
    trailer = (const uint8_t *)data + size - KBOOT_TRAILER_LEN;
    load_pa = ldl_le_p(trailer + 8);
    entry_pa = ldl_le_p(trailer + 12);
    bootargs_pa = ldl_le_p(trailer + 16);
    image_len = ldl_le_p(trailer + 20);

    if (image_len > size - KBOOT_TRAILER_LEN ||
        load_pa < S5L8940_DRAM_BASE ||
        (uint64_t)load_pa + image_len > S5L8940_DRAM_BASE + S5L8940_DRAM_SIZE) {
        error_report("n94: kboot bundle does not fit in DRAM (load 0x%x len 0x%x)",
                     load_pa, image_len);
        exit(1);
    }
    if (address_space_write(&address_space_memory, load_pa,
                            MEMTXATTRS_UNSPECIFIED, data, image_len) != MEMTX_OK) {
        error_report("n94: cannot stage kboot bundle at 0x%x", load_pa);
        exit(1);
    }
    for (gsize off = image_len; off + KBOOT_SEGMENT_LEN <= size - KBOOT_TRAILER_LEN;) {
        const uint8_t *seg = (const uint8_t *)data + off;
        uint32_t pa = ldl_le_p(seg + 8), len = ldl_le_p(seg + 12);
        bool zero = ldl_le_p(seg + 16) & 1;

        off += KBOOT_SEGMENT_LEN + (zero ? 0 : len);
        if (memcmp(seg, "K48SEG\0\0", 8) != 0 || off > size - KBOOT_TRAILER_LEN) {
            error_report("n94: malformed segment in kboot bundle");
            exit(1);
        }
        if (zero) {
            address_space_set(&address_space_memory, pa, 0, len, MEMTXATTRS_UNSPECIFIED);
        } else {
            address_space_write(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                                seg + KBOOT_SEGMENT_LEN, len);
        }
    }

    /* cpu_reset leaves us in SVC mode, IRQ/FIQ masked, MMU and caches off. */
    s->cpu->env.regs[0] = bootargs_pa;
    cpu_set_pc(cs, entry_pa);
}

/*
 * The framebuffer iBoot would have left lit, from the bundle's boot_args
 * (Video: base, display, rowbytes, width, height, depth at +20): the display
 * model scans it out from reset, so AppleH4PCLCD finds the pipe programmed.
 * tools/ios-prep puts it at the top of DRAM under pram (0x9f700000, 640x960).
 */
static bool n94_kboot_video(const char *path, uint32_t *base, uint32_t *width, uint32_t *height)
{
    g_autofree char *data = NULL;
    gsize size;
    const uint8_t *trailer, *args;
    uint32_t load_pa, bootargs_pa, image_len;

    if (!g_file_get_contents(path, &data, &size, NULL) || size < KBOOT_TRAILER_LEN ||
        memcmp(data + size - KBOOT_TRAILER_LEN, KBOOT_MAGIC, 8) != 0) {
        return false;
    }
    trailer = (const uint8_t *)data + size - KBOOT_TRAILER_LEN;
    load_pa = ldl_le_p(trailer + 8);
    bootargs_pa = ldl_le_p(trailer + 16);
    image_len = ldl_le_p(trailer + 20);
    if (bootargs_pa < load_pa || bootargs_pa - load_pa + 44 > image_len) {
        return false;
    }
    args = (const uint8_t *)data + (bootargs_pa - load_pa);
    *base = ldl_le_p(args + 20);
    *width = ldl_le_p(args + 32);
    *height = ldl_le_p(args + 36);
    return *base != 0 && *width != 0 && *height != 0;
}

static void n94_init(MachineState *machine)
{
    N94MachineState *s = N94_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj;
    DeviceState *dev;
    SysBusDevice *sbd;
    int i;

    if (!s->kboot_path) {
        error_report("n94: kboot= is required");
        exit(1);
    }

    /*
     * One Cortex-A9 for bring-up: the kboot device tree unmatches cpus/cpu1
     * (ios-prep --cpus 1), so the kernel never tries to start it.
     */
    cpuobj = object_new(machine->cpu_type);
    s->cpu = ARM_CPU(cpuobj);
    object_property_set_link(cpuobj, "memory", OBJECT(sysmem), &error_abort);
    if (object_property_find(cpuobj, "has_el3")) {
        object_property_set_bool(cpuobj, "has_el3", false, &error_fatal);
    }
    if (object_property_find(cpuobj, "has_el2")) {
        object_property_set_bool(cpuobj, "has_el2", false, &error_fatal);
    }
    object_property_set_bool(cpuobj, "realized", true, &error_fatal);
    object_unref(cpuobj);

    memory_region_init_ram(&s->dram, NULL, "n94.dram", S5L8940_DRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, S5L8940_DRAM_BASE, &s->dram);
    /*
     * PA 0 is DRAM's first page, as on the A4 boards (ipad1.c): the 6.x
     * kernel ml_io_maps PA 0 and copies its exception vectors there for a
     * core reset to land on; without a page here the copy aborts.
     */
    memory_region_init_alias(&s->dram_lo, NULL, "n94.dram-lo", &s->dram, 0, 0x1000);
    memory_region_add_subregion(sysmem, 0, &s->dram_lo);

    /*
     * Everything in the peripheral window that has no model yet. Real devices
     * added below sit on top of this at higher priority.
     */
    create_unimplemented_device("s5l8940.periph", S5L8940_PERIPH_BASE, S5L8940_PERIPH_SIZE);

    /* The L2C-310 the kernel's platform_cache function sets up. */
    sysbus_create_simple("l2x0", S5L8940_PL310_BASE, NULL);

    dev = qdev_new(TYPE_S5L8940_AIC);
    s->aic = dev;
    sbd = SYS_BUS_DEVICE(dev);
    qdev_prop_set_uint32(dev, "num-cpus", 2);
    qdev_prop_set_uint32(dev, "num-irqs", S5L8940_AIC_NUM_IRQS);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8940_AIC_BASE);
    sysbus_connect_irq(sbd, S5L8940_AIC_IRQ(0), qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(sbd, S5L8940_AIC_FIQ(0), qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ));

    /*
     * PMGR: the A4 model (PLLs, clock gates, event timers, POWER_ID). The A5
     * keeps the block's layout; its timers' AIC vectors are not in the DT, so
     * they stay unconnected until the kernel shows which it uses.
     */
    dev = qdev_new(TYPE_S5L8930_PMGR);
    s->pmgr = dev;
    sbd = SYS_BUS_DEVICE(dev);
    qdev_prop_set_uint8(dev, "board-id", 0x08);     /* N94AP */
    qdev_prop_set_uint32(dev, "gates", 0x78);       /* the kernel polls gate 0x44 and pulses 0x5c */
    qdev_prop_set_bit(dev, "clkcfg-hi", true);      /* clock configs at 0x200-0x300 too */
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8940_PMGR_BASE);

    dev = qdev_new(TYPE_S5L8930_GPIO);
    s->gpio = dev;
    sbd = SYS_BUS_DEVICE(dev);
    qdev_prop_set_uint32(dev, "ports", S5L8940_GPIO_PORTS);
    qdev_prop_set_uint32(dev, "int-groups", S5L8940_GPIO_INT_GROUPS);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8940_GPIO_BASE);
    sysbus_connect_irq(sbd, 0, n94_irq(s, S5L8940_IRQ_GPIO));

    /*
     * CDMA with its AES filter: the A4 model at the A5's addresses (same layout,
     * 44 channels where the A4 has 38; the last six fall into the unimplemented
     * window). bsd_init's IOSecureBSDRoot waits until a kext asks the platform
     * "SecureRoot", and the asker is the AES accelerator this block provides.
     */
    dev = qdev_new(TYPE_S5L8930_CDMA);
    qdev_prop_set_uint64(dev, "dram-base", S5L8940_DRAM_BASE);
    qdev_prop_set_uint64(dev, "dram-size", S5L8940_DRAM_SIZE);
    qdev_prop_set_uint32(dev, "uart-base", S5L8940_UART_BASE(0));
    qdev_prop_set_uint32(dev, "paced-base", S5L8940_I2S_BASE(0));
    qdev_prop_set_uint32(dev, "paced-ports", 4);
    qdev_prop_set_uint8(dev, "version", 3);         /* DT cdma-version */
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8940_CDMA_BASE);
    sysbus_mmio_map(sbd, 1, S5L8940_AES_BASE);
    for (i = 1; i < S5L8930_CDMA_CHANNELS; i++) {
        sysbus_connect_irq(sbd, i, n94_irq(s, S5L8940_IRQ_CDMA(i)));
    }

    /*
     * The IOP, high-level (hw/arm/s5l8940_iop.c): AppleA5IOP's control block at
     * 0x3F300000, the doorbell the AP rings through the AIC ("IPID" 3) and the
     * EmbeddedIOP rings in DRAM. AppleIOPFMI's PPN commands come to it next.
     */
    dev = qdev_new(TYPE_S5L8940_IOP);
    object_property_set_link(OBJECT(dev), "aic", OBJECT(s->aic), &error_abort);
    if (s->nand_path) {
        qdev_prop_set_string(dev, "nand", s->nand_path);
    }
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8940_IOP_BASE);
    qdev_connect_gpio_out_named(s->aic, "doorbell", 0, qdev_get_gpio_in_named(dev, "doorbell", 0));

    /*
     * The AE2 audio coprocessor, high-level (hw/arm/s5l8940_ae2.c): AppleA5AE2's control block,
     * mailbox, power and configuration blocks and SRAM, its interrupt on AIC 97. Every audio path
     * on the A5 goes through it, and mediaserverd's audio session waits for that path.
     */
    dev = qdev_new(TYPE_S5L8940_AE2);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8940_AE2_CTRL_BASE);
    sysbus_mmio_map(sbd, 1, S5L8940_AE2_MBOX_BASE);
    sysbus_mmio_map(sbd, 2, S5L8940_AE2_PWR_BASE);
    sysbus_mmio_map(sbd, 3, S5L8940_AE2_CFG_BASE);
    sysbus_mmio_map(sbd, 4, S5L8940_AE2_SRAM_BASE);
    sysbus_connect_irq(sbd, 0, n94_irq(s, S5L8940_IRQ_AE2));

    /*
     * I2S0-3, the A4's Samsung block (hw/arm/s5l8930_i2s.c) at the A5's bases (i2s-version 3).
     * AppleSamsungI2S's stop path writes +0x00 <- 0x34 and spins on bit 1 ("drained") with
     * IODelay; on the unimplemented window it pinned the CPU from the codec's start on.
     * ponytail: register files with no host audio yet; give i2s0 "audio-out" once the AE2
     * path carries PCM.
     */
    for (i = 0; i < 4; i++) {
        dev = qdev_new(TYPE_S5L8930_I2S);
        qdev_prop_set_uint8(dev, "port", i);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, S5L8940_I2S_BASE(i));
    }

    /*
     * I2C controllers (i2c,s5l8940x; hw/arm/s5l8940_i2c.c) and the DT's slaves.
     * The PMU driver's first read of the D1881 at 0x74 timing out on an
     * unimplemented block was a kernel panic, and with the bus there but
     * empty it retried the read forever. The other parts reuse the A4 boards'
     * models for the same chips (the codec driver never checks its ID).
     */
    {
        static const struct {
            uint8_t bus, addr;
            const char *type;
        } slaves[] = {
            { 0, 0x74, TYPE_S5L8940_D1881 },     /* pmu,d1881 */
            { 0, 0x4a, TYPE_CS42L58 },            /* audio-control,cs42l63: a MAP register file */
            { 1, 0x49, TYPE_S5L8930_TSL2581 },    /* als,tsl2581 */
            { 1, 0x0d, TYPE_S5L8930_AK8975 },     /* compass,akm8975b */
            { 2, 0x19, TYPE_LIS302DL },           /* accelerometer,lis331dlh */
            { 2, 0x6a, TYPE_S5L8930_L3G4200D },   /* gyro,ap3gdl */
        };
        I2CBus *bus[3];

        for (i = 0; i < 3; i++) {
            dev = qdev_new(TYPE_S5L8940_I2C);
            sbd = SYS_BUS_DEVICE(dev);
            sysbus_realize_and_unref(sbd, &error_fatal);
            sysbus_mmio_map(sbd, 0, S5L8940_I2C_BASE(i));
            sysbus_connect_irq(sbd, 0, n94_irq(s, S5L8940_IRQ_I2C(i)));
            bus[i] = I2C_BUS(qdev_get_child_bus(dev, "i2c"));
        }
        for (i = 0; i < ARRAY_SIZE(slaves); i++) {
            I2CSlave *slave = i2c_slave_new(slaves[i].type, slaves[i].addr);

            if (!strcmp(slaves[i].type, TYPE_LIS302DL)) {
                qdev_prop_set_uint8(DEVICE(slave), "whoami", 0x32);   /* LIS331DLH */
                qdev_prop_set_string(DEVICE(slave), "mount", "-2,1,3");
            }
            if (!strcmp(slaves[i].type, TYPE_S5L8930_L3G4200D)) {
                qdev_prop_set_string(DEVICE(slave), "mount", "1,2,3");
            }
            i2c_slave_realize_and_unref(slave, bus[slaves[i].bus], &error_fatal);
            if (!strcmp(slaves[i].type, TYPE_S5L8940_D1881)) {
                /* the PMU interrupts on GPIO 0x0d, active low (DT pmu interrupts {0xd, 1}) */
                qdev_connect_gpio_out(DEVICE(slave), 0,
                                      qemu_irq_invert(qdev_get_gpio_in(s->gpio, S5L8930_GPIO_PIN(0x00d))));
            }
        }
    }

    /*
     * USB, as ipad1.c: the iPod's PHY register file and Synopsys DWC OTG core
     * in device mode (the A5 keeps both layouts; GHWCFG as the A4's), with
     * the OTG's built-in host enumerating the device when no usbmuxd bridge
     * is dialed (IT_USB_TCP=host:port), plus plain EHCI and OHCI for the host
     * side. The restore ramdisk's restored_external brings the stack up and
     * the EHCI's init panicked on the unimplemented window.
     */
    {
        static uint32_t hwcfg[] = { 0, 0x7a8f60d0, 0x082000e8, 0x01f08024 };

        dev = qdev_new(TYPE_IPOD_TOUCH_USB_PHYS);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, S5L8940_USB_PHY_BASE);

        dev = ipod_touch_init_usb_otg(n94_irq(s, S5L8940_IRQ_USB_OTG), hwcfg);
        s->usb_otg = S5L8900USBOTG(dev);
        synopsys_usb_set_tcp_addr(s->usb_otg, NULL);
        s->usb_otg->builtin_host = !s->usb_otg->server_host && !getenv("IT_USB_TCP");
        s->usb_otg->host_charge = true;
        s->usb_otg->withhold_charge = false;
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        memory_region_add_subregion(sysmem, S5L8940_USB_OTG_BASE, &s->usb_otg->iomem);

        dev = qdev_new(TYPE_EXYNOS4210_EHCI);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, S5L8940_USB_EHCI_BASE);
        sysbus_connect_irq(sbd, 0, n94_irq(s, S5L8940_IRQ_USB_EHCI));

        for (i = 0; i < 2; i++) {
            dev = qdev_new(TYPE_SYSBUS_OHCI);
            qdev_prop_set_uint32(dev, "num-ports", 1);      /* DT port-count */
            sbd = SYS_BUS_DEVICE(dev);
            sysbus_realize_and_unref(sbd, &error_fatal);
            sysbus_mmio_map(sbd, 0, i ? S5L8940_USB_OHCI1_BASE : S5L8940_USB_OHCI0_BASE);
            sysbus_connect_irq(sbd, 0, n94_irq(s, i ? S5L8940_IRQ_USB_OHCI1 : S5L8940_IRQ_USB_OHCI0));
        }
    }

    /*
     * The panel: the A4 display model (DisplayPipe0, CLCD, RGBOUT and its
     * control block, TV-out) at the A5's addresses, the same register offsets
     * (AppleH4PDisplayPipe/AppleH4PCLCD are the A5 builds of the A4 drivers),
     * scanning out to the QEMU console. Its reset leaves the block as iBoot
     * hands it over, UI0 live on boot_args' framebuffer and the CLCD's timing
     * generator running: the 6.x AppleCLCD::start_hardware adopts only that
     * ("CLCD not initied by iBoot, driver will not load" otherwise) and
     * without it IOMobileFramebuffer has no main display for backboardd.
     * The clcd node has no iommu-parent (fMapper 0), so the pipe's layer bases
     * are physical addresses and the model's dart2 is left unmapped.
     */
    {
        uint32_t fb_base = 0x9f700000, fb_w = 640, fb_h = 960;

        if (!n94_kboot_video(s->kboot_path, &fb_base, &fb_w, &fb_h)) {
            warn_report("n94: no Video in the kboot bundle's boot_args, assuming 640x960 at 0x9f700000");
        }
        dev = qdev_new(TYPE_S5L8930_DISPLAY);
        s->display = dev;
        qdev_prop_set_uint64(dev, "fb-base", fb_base);
        qdev_prop_set_uint16(dev, "width", fb_w);
        qdev_prop_set_uint16(dev, "height", fb_h);
        qdev_prop_set_uint8(dev, "generation", 4);      /* AppleH4PDisplayPipe's window layout and layer MMU */
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, S5L8940_DISP_PIPE0_BASE);
        sysbus_mmio_map(sbd, 1, S5L8940_CLCD_BASE);
        sysbus_mmio_map(sbd, 3, S5L8940_RGBOUT_BASE);
        sysbus_mmio_map(sbd, 4, S5L8940_TVOUT_BASE);
        sysbus_mmio_map(sbd, 5, S5L8940_RGBOUT2_BASE);
        sysbus_connect_irq(sbd, 0, n94_irq(s, S5L8940_IRQ_DISP_PIPE0));
        sysbus_connect_irq(sbd, 1, n94_irq(s, S5L8940_IRQ_CLCD));
        sysbus_connect_irq(sbd, 2, n94_irq(s, S5L8940_IRQ_RGBOUT_PIPE));

        /*
         * MIPI-DSIM, the Samsung IP the A4 boards share with the iPod, with the
         * link as iBoot's pinot_init leaves it (HS clock running). The panel
         * ID ApplePinotLCD wants is the DT's lcd-panel-id (tools/ios-prep
         * writes it); the kernel never reads it over the link.
         */
        dev = qdev_new(TYPE_IPOD_TOUCH_MIPI_DSI);
        IPOD_TOUCH_MIPI_DSI(dev)->hs_clock_at_reset = true;
        qdev_prop_set_uint32(dev, "lanes", 4);            /* DT #lanes */
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, S5L8940_DSIM_BASE);
        sysbus_connect_irq(sbd, 0, n94_irq(s, S5L8940_IRQ_DSIM));

        /* The two DARTs AppleS5L8930XDART programs at boot (jpeg/scaler, isp). */
        sysbus_create_simple(TYPE_S5L8930_DART, S5L8940_DART_NRT_BASE, NULL);
        sysbus_create_simple(TYPE_S5L8930_DART, S5L8940_DART_RT_BASE, NULL);
    }

    /*
     * Samsung UARTs, the four the N94 device tree has: uart0 is the kernel's
     * console (-serial), uart1 its debug port (the second -serial); uart3
     * (Bluetooth HCI) and uart5 (the gas gauge's HDQ) are silent: nothing is
     * attached, as on the fork's A4 boards, but the block must exist, or
     * AppleOnboardSerial's rx DMA path reads zeros where it expects FIFO
     * status and panics on its queue accounting (AppleSimpleUARTSync.cpp:2177)
     * when BlueTool opens uart3.
     */
    for (i = 0; i < 6; i++) {
        if (i == 2 || i == 4) {
            continue;
        }
        exynos4210_uart_create(S5L8940_UART_BASE(i), 256, i, i < 2 ? serial_hd(i) : NULL,
                               n94_irq(s, S5L8940_IRQ_UART(i)), true);
    }

    qemu_register_reset(n94_cpu_reset, s);
}

static char *n94_get_kboot(Object *obj, Error **errp)
{
    return g_strdup(N94_MACHINE(obj)->kboot_path);
}

static void n94_set_kboot(Object *obj, const char *value, Error **errp)
{
    N94MachineState *s = N94_MACHINE(obj);

    g_free(s->kboot_path);
    s->kboot_path = g_strdup(value);
}

static char *n94_get_nand(Object *obj, Error **errp)
{
    return g_strdup(N94_MACHINE(obj)->nand_path);
}

static void n94_set_nand(Object *obj, const char *value, Error **errp)
{
    N94MachineState *s = N94_MACHINE(obj);

    g_free(s->nand_path);
    s->nand_path = g_strdup(value);
}

static void n94_instance_finalize(Object *obj)
{
    g_free(N94_MACHINE(obj)->kboot_path);
    g_free(N94_MACHINE(obj)->nand_path);
}

static void n94_class_init(ObjectClass *klass, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);

    mc->desc = "iPhone 4S (N94AP, S5L8940X)";
    mc->init = n94_init;
    mc->max_cpus = 2;
    mc->default_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a9");
    mc->minimum_page_bits = 12;
    mc->default_ram_size = S5L8940_DRAM_SIZE;
    mc->no_cdrom = 1;
    mc->no_floppy = 1;
    mc->no_parallel = 1;

    object_class_property_add_str(klass, "kboot", n94_get_kboot, n94_set_kboot);
    object_class_property_set_description(klass, "kboot",
        "K48KBOOT bundle from tools/ios-prep (required)");
    object_class_property_add_str(klass, "nand", n94_get_nand, n94_set_nand);
    object_class_property_set_description(klass, "nand",
        "PPN page-store directory (geometry.json + ch<n>-ce<m>.pages, tools/ios-prep nand); blank chips if unset");
}

static const TypeInfo n94_machine_info = {
    .name = TYPE_N94_MACHINE,
    .parent = TYPE_MACHINE,
    .interfaces = arm_machine_interfaces,
    .instance_size = sizeof(N94MachineState),
    .instance_finalize = n94_instance_finalize,
    .class_init = n94_class_init,
};

static void n94_machine_types(void)
{
    type_register_static(&n94_machine_info);
}

type_init(n94_machine_types)
