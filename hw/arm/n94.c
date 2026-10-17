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
    char *kboot_path;
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

    /* Samsung UARTs: uart0 is the kernel's console (-serial), uart1 its debug port. */
    for (i = 0; i < 2; i++) {
        exynos4210_uart_create(S5L8940_UART_BASE(i), 256, i, serial_hd(i),
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

static void n94_instance_finalize(Object *obj)
{
    g_free(N94_MACHINE(obj)->kboot_path);
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
