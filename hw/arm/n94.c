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
#include "hw/arm/ipod_touch_spi.h"
#include "hw/usb/hcd-ehci.h"
#include "hw/usb/hcd-ohci.h"
#include "system/reset.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/cpregs.h"
#include "target/arm/arm-powerctl.h"
#include "hw/arm/guest-services/general.h"
#include "hw/arm/guest-services/gles.h"
#include "hw/arm/guest-package.h"
#include "ui/console.h"
#include "ui/input.h"
#include "qapi/visitor.h"
#include "chardev/char.h"

#define TYPE_N94_MACHINE MACHINE_TYPE_NAME("iPhone-4S")
#define N94_MAX_CPUS 2                   /* the A5's two Cortex-A9s (DT cpus/cpu0, cpu1) */
OBJECT_DECLARE_SIMPLE_TYPE(N94MachineState, N94_MACHINE)

struct N94MachineState {
    MachineState parent;
    ARMCPU *cpu;                         /* cpu0, the boot core */
    ARMCPU *cpus[N94_MAX_CPUS];
    MemoryRegion dram;
    MemoryRegion dram_lo;                /* DRAM's first page at PA 0 (the kernel's reset-vector page) */
    DeviceState *aic;
    DeviceState *pmgr;
    DeviceState *gpio;
    DeviceState *display;                /* the display pipe, CLCD, RGBOUT, TV-out (hw/arm/s5l8930_display.c) */
    synopsys_usb_state *usb_otg;
    MemoryRegion sac;                    /* the panel's adaptive clocking block (n94_sac_ops) */
    uint32_t sac_regs[0x40];
    struct N94RegFile *dwi, *pl310_cif, *armio[2];
    Chardev *gauge;                      /* the bq27540 on uart5's HDQ line (s5l8930_hdq.c) */
    Object *pmu;                         /* the D1881 (its vbat-mv follows battery-level) */
    int battery_level;                   /* %, battery-level property */
    IPodTouchMultitouchState *mt;        /* the digitizer on spi1 */
    bool kbd_cmd, kbd_shift;
    int kbd_btn_held[Q_KEY_CODE__MAX];   /* qcode -> 1 + button pin */
    int mtt_x[MT_MAX_FINGERS], mtt_y[MT_MAX_FINGERS];
    bool mtt_seen[MT_MAX_FINGERS];
    char *kboot_path;
    char *nand_path;                     /* nand=: the PPN page store directory (tools/ios-prep nand) */
    GuestPackage pkg;                    /* the GL front end's hello, guest packages (hw/arm/guest-package.c) */
    CPUState *gl_owner;                  /* the vCPU whose host thread has the GL contexts current */
};

static void n94_battery_update(N94MachineState *s);

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

    for (int i = 0; i < N94_MAX_CPUS; i++) {
        if (s->cpus[i]) {
            cpu_reset(CPU(s->cpus[i]));      /* cpu1 comes out of reset powered off */
        }
    }
    /* A reboot sends no GL teardown for its old processes. */
    gles_host_reset();
    guest_pkg_reset(&s->pkg);

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

/*
 * Touch from the host, as ipad1.c does it for the A4 boards: the QEMU pointer (absolute,
 * 0..32767 over the console, the console being the 640x960 panel in portrait) is finger 0,
 * the left button pressing it; QMP input-send-event "mtt" events carry up to five fingers.
 * The digitizer's y runs bottom-up.
 */
static void n94_mouse_event(void *opaque, int x, int y, int z, int buttons)
{
    IPodTouchMultitouchState *mt = opaque;

    mt->touch_x = x / 32768.0f;
    mt->touch_y = 1.0f - y / 32768.0f;
    if (buttons && !mt->touch_down) {
        ipod_touch_multitouch_on_touch(mt);
    } else if (!buttons && mt->touch_down) {
        ipod_touch_multitouch_on_release(mt);
    } else if (buttons) {
        ipod_touch_multitouch_on_motion(mt);
    }
}

static void n94_mtt_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    N94MachineState *s = N94_MACHINE(qdev_get_machine());
    InputMultiTouchEvent *mtt = evt->u.mtt.data;
    int slot = mtt->slot;
    bool down;

    if (slot < 0 || slot >= MT_MAX_FINGERS) {
        return;
    }
    switch (mtt->type) {
    case INPUT_MULTI_TOUCH_TYPE_DATA:
        if (mtt->axis == INPUT_AXIS_X) {
            s->mtt_x[slot] = mtt->value;
        } else {
            s->mtt_y[slot] = mtt->value;
        }
        s->mtt_seen[slot] = true;
        return;
    case INPUT_MULTI_TOUCH_TYPE_BEGIN:
    case INPUT_MULTI_TOUCH_TYPE_UPDATE:
    case INPUT_MULTI_TOUCH_TYPE_END:
    case INPUT_MULTI_TOUCH_TYPE_CANCEL:
        if (!s->mtt_seen[slot]) {
            return;
        }
        down = mtt->type == INPUT_MULTI_TOUCH_TYPE_BEGIN ||
               mtt->type == INPUT_MULTI_TOUCH_TYPE_UPDATE;
        ipod_touch_multitouch_set_finger(s->mt, slot, s->mtt_x[slot] / 32768.0f,
                                         1.0f - s->mtt_y[slot] / 32768.0f, down);
        if (!down) {
            s->mtt_seen[slot] = false;
        }
        return;
    default:
        return;
    }
}

static const QemuInputHandler n94_mtt_handler = {
    .name  = "iPhone 4S Multitouch",
    .mask  = INPUT_EVENT_MASK_MTT,
    .event = n94_mtt_event,
};

/*
 * The buttons node's GPIO inputs (AppleM68Buttons): menu 0, hold 1, volume down 2, up 3,
 * ring/silent 4, active low, resting high in the GPIO model. Host keys as on the A4 boards:
 * Cmd+Shift+H home, Cmd+L hold/power, Cmd+- and Cmd+= volume; Home, End, Page Down and
 * Page Up do the same without a modifier (the Windows key rarely reaches the SDL window),
 * so the monitor's sendkey works too. A key that pressed a button always releases it.
 */
static void n94_set_button(N94MachineState *s, int pin, bool down)
{
    qemu_set_irq(qdev_get_gpio_in(s->gpio, pin), !down);
}

static void n94_kbd_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    N94MachineState *s = N94_MACHINE(qdev_get_machine());
    int q = qemu_input_key_value_to_qcode(evt->u.key.data->key);
    bool down = evt->u.key.data->down;
    int pin = -1;

    switch (q) {
    case Q_KEY_CODE_META_L:
    case Q_KEY_CODE_META_R:
        s->kbd_cmd = down;
        return;
    case Q_KEY_CODE_SHIFT:
    case Q_KEY_CODE_SHIFT_R:
        s->kbd_shift = down;
        return;
    default:
        break;
    }
    if (q < 0 || q >= Q_KEY_CODE__MAX) {
        return;
    }
    if (!down && s->kbd_btn_held[q]) {
        pin = s->kbd_btn_held[q] - 1;
    } else if (down && s->kbd_cmd) {
        switch (q) {
        case Q_KEY_CODE_L:     pin = S5L8940_GPIO_BTN_HOLD; break;
        case Q_KEY_CODE_H:     if (s->kbd_shift) pin = S5L8940_GPIO_BTN_MENU; break;
        case Q_KEY_CODE_MINUS: pin = S5L8940_GPIO_BTN_VOLDOWN; break;
        case Q_KEY_CODE_EQUAL: pin = S5L8940_GPIO_BTN_VOLUP; break;
        default: break;
        }
    } else if (down) {
        switch (q) {
        case Q_KEY_CODE_HOME:  pin = S5L8940_GPIO_BTN_MENU; break;
        case Q_KEY_CODE_END:   pin = S5L8940_GPIO_BTN_HOLD; break;
        case Q_KEY_CODE_PGDN:  pin = S5L8940_GPIO_BTN_VOLDOWN; break;
        case Q_KEY_CODE_PGUP:  pin = S5L8940_GPIO_BTN_VOLUP; break;
        default: break;
        }
    }
    if (pin < 0) {
        return;
    }
    s->kbd_btn_held[q] = down ? pin + 1 : 0;
    n94_set_button(s, pin, down);
}

static const QemuInputHandler n94_kbd_handler = {
    .name  = "iPhone 4S Buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = n94_kbd_event,
};

/*
 * The panel's slow adaptive clocking block (mipi-dsim reg[1], 0x3F110000, sac-version 1; the
 * sacm node's "saca" function 'lcd0'). AppleSamsungMIPIDSIController and the sacm manager write
 * +0x24/+0x20 (the clock step) and +0x00 <- 0x10000001, then spin with no timeout until +0x00
 * bit 8 (ready) reads set before writing 0x10000003. The first boot finds the link as iBoot
 * left it and never waits; every panel power-on after an auto-lock does, and on the unimplemented
 * window it hung the kernel with the LCD on and the screen black. A register file whose +0x00
 * always reads ready.
 */
#define N94_SAC_READY 0x100

static uint64_t n94_sac_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t *regs = opaque;

    return regs[addr >> 2] | (addr == 0 ? N94_SAC_READY : 0);
}

static void n94_sac_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    uint32_t *regs = opaque;

    regs[addr >> 2] = val;
}

/*
 * Blocks the kernel only configures, as register files that read back what was written, with
 * the bits a driver polls for "done" reading clear (the operation is over at once):
 *
 * - dwi (0x3F700000, dwi,s5l8940x; AppleS5L8940XDWI, the D1881's buck control by single-wire
 *   interface, "using DWI for BUCK changes"): three channels with their control word at +0x10,
 *   +0x20 and +0x40 (0x71 starts a transfer, bit 0 busy, polled with a deadline and a panic) and
 *   their data at +0x14, +0x24/+0x30 and +0x44; +0x00, +0x04 and +0x50 are timing set at start.
 *   The model does not forward the buck settings to the PMU.
 * - The PL310's second window (pl310 reg[1], 0x3FD00000, 0x2000: AppleS5L8940XPL310's "cif"):
 *   0x1020 <- 1 and 0x1120 <- 0x80000100 at start.
 * - arm-io's own reg (0x3FB00000 and 0x3FC00000, 0x1000 each; AppleS5L8940XIO): the two fabrics'
 *   bus timeout (+0x04 <- the DT's bus-timeout 0x7fffff, then 0x01000000) and PIO error reporting
 *   (+0x18 <- 1, +0x1c <- 3); no error is ever latched.
 */
typedef struct N94RegFile {
    MemoryRegion mr;
    uint32_t done_mask[6];               /* {offset, bits that read 0} pairs, offset 0 ends */
    uint32_t regs[];
} N94RegFile;

static uint64_t n94_regfile_read(void *opaque, hwaddr addr, unsigned size)
{
    N94RegFile *r = opaque;
    uint32_t val = r->regs[addr >> 2];

    for (unsigned i = 0; i + 1 < ARRAY_SIZE(r->done_mask) && r->done_mask[i]; i += 2) {
        if (addr == r->done_mask[i]) {
            val &= ~r->done_mask[i + 1];
        }
    }
    return val;
}

static void n94_regfile_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    N94RegFile *r = opaque;

    r->regs[addr >> 2] = val;
}

static const MemoryRegionOps n94_regfile_ops = {
    .read = n94_regfile_read,
    .write = n94_regfile_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static N94RegFile *n94_regfile(N94MachineState *s, const char *name, hwaddr base, uint32_t size,
                               const uint32_t *done, unsigned ndone)
{
    N94RegFile *r = g_malloc0(sizeof(*r) + size);

    memcpy(r->done_mask, done, MIN(ndone, ARRAY_SIZE(r->done_mask)) * sizeof(uint32_t));
    memory_region_init_io(&r->mr, OBJECT(s), &n94_regfile_ops, r, name, size);
    memory_region_add_subregion(get_system_memory(), base, &r->mr);
    return r;
}

static const MemoryRegionOps n94_sac_ops = {
    .read = n94_sac_read,
    .write = n94_sac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/*
 * The guest-services trap (mcr p15,3,Rn,c15,c15,0) the GL front end
 * (contrib/gles-public, OpenGLES.framework) calls the host through, as on the
 * A4 boards (ipad1.c): GL calls go to the host executor, hw/arm/gles-host.c,
 * which runs them on ANGLE on Windows (docs/windows-gl.md). GLES, its liveness
 * probe, and the front end's hello and guest packages (guest-package.c); the A4
 * boards' pasteboard and agent are not wired.
 */
static void n94_qemu_call_on(CPUState *cs, uint64_t value)
{
    N94MachineState *s = N94_MACHINE(qdev_get_machine());
    qemu_call_t q;
    int32_t err = 0;

    if (cpu_memory_rw_debug(cs, value, (uint8_t *)&q, sizeof(q), 0)) {
        return;
    }
    switch (q.call_number) {
    case QC_GLES:
        q.retval = qc_handle_gles(cs, &q.args.gles);
        break;
    case QC_GLES_PING:
        q.retval = QC_GLES_PING_MAGIC;
        break;
    default:
        if (!guest_pkg_call(&s->pkg, cs, &q, &err)) {
            return;
        }
    }
    q.error = err;
    cpu_memory_rw_debug(cs, value, (uint8_t *)&q, sizeof(q), 1);
}

static void n94_gl_release_work(CPUState *cpu, run_on_cpu_data data)
{
    gles_host_release_thread();
}

/*
 * The calls run on the trapping vCPU's thread: a GL call may raise a guest exception on its caller
 * (ARM_CP_RAISES_EXC, a page to fault in), which only that thread can take. The host's GL contexts
 * follow: an EGL context is current on one thread at a time, and with the two vCPUs on threads of
 * their own (MTTCG) ANGLE refused the context on cpu1's and CoreAnimation drew nothing. So a call
 * from a vCPU other than the one whose thread holds the contexts first has that thread let go of
 * them (gles_host_release_thread, run there) and waits; it counts as out of its execution loop
 * meanwhile, since the holder may itself be waiting for exclusive work (the guest's broadcast TLB
 * maintenance) that needs every vCPU outside it.
 */
static void n94_qemu_call(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    CPUState *cs = env_cpu(env);
    N94MachineState *s = N94_MACHINE(qdev_get_machine());

    if (s->gl_owner && s->gl_owner != cs) {
        cpu_exec_end(cs);
        run_on_cpu(s->gl_owner, n94_gl_release_work, RUN_ON_CPU_NULL);
        cpu_exec_start(cs);
    }
    s->gl_owner = cs;
    n94_qemu_call_on(cs, value);
}

static const ARMCPRegInfo n94_cp_reginfo[] = {
    { .name = "QEMU_CALL", .cp = 15, .opc1 = 3, .crn = 15, .crm = 15,
      .opc2 = 0, .access = PL0_RW, .state = ARM_CP_STATE_AA32,
      .type = ARM_CP_IO | ARM_CP_NO_RAW | ARM_CP_RAISES_EXC, /* gles_guest_rw */
      .readfn = qemu_call_status,
      .writefn = n94_qemu_call },
};

/*
 * The PMGR's core control (enable_core): core n powers on at its reset vector, PA 0, where the
 * kernel has copied its exception vectors (its reset handler takes the secondary from there), in
 * SVC mode with the MMU off, as a core leaving reset on the A5; or powers off.
 */
static void n94_core_enable(void *opaque, int n, int level)
{
    N94MachineState *s = opaque;
    int ret;

    if (n == 0 || n >= N94_MAX_CPUS || !s->cpus[n]) {
        return;
    }
    if (level) {
        ret = arm_set_cpu_on(s->cpus[n]->mp_affinity, 0, 0, 1, false);
    } else {
        ret = arm_set_cpu_off(s->cpus[n]->mp_affinity);
    }
    if (ret != QEMU_ARM_POWERCTL_RET_SUCCESS && ret != QEMU_ARM_POWERCTL_ALREADY_ON &&
        ret != QEMU_ARM_POWERCTL_IS_OFF) {
        warn_report("n94: cpu%d power %s failed (%d)", n, level ? "on" : "off", ret);
    }
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
     * The Cortex-A9s, -smp 1 or 2. cpu1 comes out of reset powered off, as on the
     * A5, until the kernel's enable_core (the PMGR) starts it. With -smp 1 the kboot
     * device tree must not show cpu1 (ios-prep --cpus 1), or the kernel waits for it.
     */
    for (i = 0; i < machine->smp.cpus; i++) {
        cpuobj = object_new(machine->cpu_type);
        s->cpus[i] = ARM_CPU(cpuobj);
        object_property_set_link(cpuobj, "memory", OBJECT(sysmem), &error_abort);
        if (object_property_find(cpuobj, "has_el3")) {
            object_property_set_bool(cpuobj, "has_el3", false, &error_fatal);
        }
        if (object_property_find(cpuobj, "has_el2")) {
            object_property_set_bool(cpuobj, "has_el2", false, &error_fatal);
        }
        object_property_set_bool(cpuobj, "start-powered-off", i > 0, &error_fatal);
        object_property_set_bool(cpuobj, "realized", true, &error_fatal);
        define_arm_cp_regs(s->cpus[i], n94_cp_reginfo);
        object_unref(cpuobj);
    }
    s->cpu = s->cpus[0];

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

    /* The L2C-310 the kernel's platform_cache function sets up, and its second window. */
    sysbus_create_simple("l2x0", S5L8940_PL310_BASE, NULL);
    s->pl310_cif = n94_regfile(s, "n94.pl310-cif", S5L8940_PL310_AUX_BASE, 0x2000, NULL, 0);
    s->armio[0] = n94_regfile(s, "n94.arm-io0", S5L8940_ARMIO_BASE(0), 0x1000, NULL, 0);
    s->armio[1] = n94_regfile(s, "n94.arm-io1", S5L8940_ARMIO_BASE(1), 0x1000, NULL, 0);
    {
        static const uint32_t dwi_done[] = { 0x10, 1, 0x20, 1, 0x40, 1 };

        s->dwi = n94_regfile(s, "n94.dwi", S5L8940_DWI_BASE, 0x1000, dwi_done, ARRAY_SIZE(dwi_done));
    }

    dev = qdev_new(TYPE_S5L8940_AIC);
    s->aic = dev;
    sbd = SYS_BUS_DEVICE(dev);
    qdev_prop_set_uint32(dev, "num-cpus", 2);
    qdev_prop_set_uint32(dev, "num-irqs", S5L8940_AIC_NUM_IRQS);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8940_AIC_BASE);
    for (i = 0; i < machine->smp.cpus; i++) {
        sysbus_connect_irq(sbd, S5L8940_AIC_IRQ(i), qdev_get_gpio_in(DEVICE(s->cpus[i]), ARM_CPU_IRQ));
        sysbus_connect_irq(sbd, S5L8940_AIC_FIQ(i), qdev_get_gpio_in(DEVICE(s->cpus[i]), ARM_CPU_FIQ));
    }

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
    qdev_prop_set_uint32(dev, "cores", N94_MAX_CPUS);
    qdev_prop_set_bit(dev, "a5", true);             /* the A5's watchdog (+0x3020) and registers */
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8940_PMGR_BASE);
    sysbus_connect_irq(sbd, 2, n94_irq(s, S5L8940_IRQ_WDT));
    for (i = 0; i < N94_MAX_CPUS; i++) {
        qdev_connect_gpio_out_named(dev, "core-enable", i,
                                    qemu_allocate_irq(n94_core_enable, s, i));
    }

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
    object_property_add_child(OBJECT(machine), "iop", OBJECT(dev));
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
            { 0, 0x4a, TYPE_CS42L63 },            /* audio-control,cs42l63: a MAP register file */
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
                s->pmu = OBJECT(slave);
                n94_battery_update(s);
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
        memory_region_init_io(&s->sac, OBJECT(s), &n94_sac_ops, s->sac_regs, "n94.sac",
                              sizeof(s->sac_regs));
        memory_region_add_subregion(sysmem, S5L8940_SAC_BASE, &s->sac);

        /* The two DARTs AppleS5L8930XDART programs at boot (jpeg/scaler, isp). */
        sysbus_create_simple(TYPE_S5L8930_DART, S5L8940_DART_NRT_BASE, NULL);
        sysbus_create_simple(TYPE_S5L8930_DART, S5L8940_DART_RT_BASE, NULL);
    }

    /*
     * spi1 and the multitouch digitizer on it: the A4 SPI block (AppleSamsungSPI drives the same
     * Samsung IP) and the N1 model the N90 and N81 use, with the N94's sensor profile. The kernel
     * sends N94.mtprops' N1F56 firmware (56264 bytes) in one CDMA burst into TXDATA, so the TX FIFO
     * holds 64 KiB. ATN interrupts on GPIO 0x15; its reset (0x407) and the PMU's LDOs are not
     * modeled (the digitizer is always powered).
     */
    dev = qdev_new(TYPE_IPOD_TOUCH_SPI);
    qdev_prop_set_uint8(dev, "index", 1);
    qdev_prop_set_string(dev, "peripheral", "multitouch");
    qdev_prop_set_uint32(dev, "tx-fifo-depth", 0x10000);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8940_SPI_BASE(1));
    sysbus_connect_irq(sbd, 0, n94_irq(s, S5L8940_IRQ_SPI(1)));
    s->mt = IPOD_TOUCH_SPI(dev)->mt;
    s->mt->profile = &mt_profile_n94;
    qdev_connect_gpio_out_named(DEVICE(s->mt), "atn", 0,
                                qdev_get_gpio_in(s->gpio, S5L8940_GPIO_MT_ATN));
    qemu_add_mouse_event_handler(n94_mouse_event, s->mt, 1, "iPhone 4S Touchscreen");
    qemu_input_handler_register(s->gpio, &n94_kbd_handler);
    qemu_input_handler_register(s->gpio, &n94_mtt_handler);

    /*
     * Samsung UARTs, the four the N94 device tree has: uart0 is the kernel's
     * console (-serial), uart1 its debug port (the second -serial); uart3
     * (Bluetooth HCI) is silent: nothing is attached, as on the fork's A4
     * boards, but the block must exist, or AppleOnboardSerial's rx DMA path
     * reads zeros where it expects FIFO status and panics on its queue
     * accounting (AppleSimpleUARTSync.cpp:2177) when BlueTool opens uart3.
     */
    for (i = 0; i < 4; i++) {
        if (i == 2) {
            continue;
        }
        exynos4210_uart_create(S5L8940_UART_BASE(i), 256, i, i < 2 ? serial_hd(i) : NULL,
                               n94_irq(s, S5L8940_IRQ_UART(i)), true);
    }
    /*
     * uart5 carries the battery's gas gauge (DT uart5/gas-gauge, gas-gauge,bq27540|gas-gauge,hdq): the
     * iPhone 4's bq27540 model the n90 board uses (hw/arm/s5l8930_hdq.c), which configd's
     * AppleHDQGasGauge plugin polls over HDQ; that is what SpringBoard's battery shows. A 16-byte FIFO
     * as the silicon's (ipad1.c has why). Without it the status bar showed the D1881 power source's
     * one-off estimate from the battery voltage. 1432 mAh, the 4S battery's; the level is the
     * battery-level property (default 80), charging while the USB host's cable is in (always here).
     */
    s->gauge = qemu_chardev_new(NULL, TYPE_CHARDEV_S5L8930_HDQ, NULL, NULL, &error_abort);
    s5l8930_hdq_set_capacity(s->gauge, 1432);
    s5l8930_hdq_set_device_type(s->gauge, 0x0540);
    s5l8930_hdq_set_battery(s->gauge, s->battery_level, true);
    exynos4210_uart_create(S5L8940_UART_BASE(5), 16, 5, s->gauge, n94_irq(s, S5L8940_IRQ_UART(5)), true);

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

/*
 * The battery's state of charge goes to the gas gauge and to the D1881's battery voltage (ADC mux 4,
 * which AppleD1881PMUPowerSource reads at boot and looks up in its battery-data curve, family 3: 4037 mV
 * read ~80% and 3500 mV ~2%, so a line through those two, clamped at 3500-4200 mV). configd's
 * AppleHDQGasGauge reads the gauge's firmware version at start and then waits for something that was
 * not seen to come in six minutes. Which of the two the status bar shows is not settled: on the Setup
 * screen the charging battery reads full at 30% either way (docs/n94/hardware.md "PMU and sensors").
 */
static void n94_battery_update(N94MachineState *s)
{
    int mv = 3500 + (s->battery_level - 2) * (4037 - 3500) / (80 - 2);

    if (s->pmu) {
        object_property_set_uint(s->pmu, "vbat-mv", MIN(MAX(mv, 3500), 4200), &error_abort);
    }
    if (s->gauge) {
        s5l8930_hdq_set_battery(s->gauge, s->battery_level, true);
    }
}

static void n94_get_battery_level(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    int64_t value = N94_MACHINE(obj)->battery_level;

    visit_type_int(v, name, &value, errp);
}

static void n94_set_battery_level(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    N94MachineState *s = N94_MACHINE(obj);
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    if (value < 0 || value > 100) {
        error_setg(errp, "battery-level must be between 0 and 100");
        return;
    }
    s->battery_level = value;
    n94_battery_update(s);
}

static void n94_instance_init(Object *obj)
{
    N94_MACHINE(obj)->battery_level = 80;
    guest_pkg_init(&N94_MACHINE(obj)->pkg, obj);
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
    mc->max_cpus = N94_MAX_CPUS;
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
    object_class_property_add(klass, "battery-level", "int", n94_get_battery_level, n94_set_battery_level,
                              NULL, NULL);
    object_class_property_set_description(klass, "battery-level",
        "the gas gauge's state of charge, 0-100 (default 80); settable at run time over QMP qom-set");
    object_class_property_add_str(klass, "nand", n94_get_nand, n94_set_nand);
    object_class_property_set_description(klass, "nand",
        "PPN page-store directory (geometry.json + ch<n>-ce<m>.pages, tools/ios-prep nand); blank chips if unset");
}

static const TypeInfo n94_machine_info = {
    .name = TYPE_N94_MACHINE,
    .parent = TYPE_MACHINE,
    .interfaces = arm_machine_interfaces,
    .instance_size = sizeof(N94MachineState),
    .instance_init = n94_instance_init,
    .instance_finalize = n94_instance_finalize,
    .class_init = n94_class_init,
};

static void n94_machine_types(void)
{
    type_register_static(&n94_machine_info);
}

type_init(n94_machine_types)
