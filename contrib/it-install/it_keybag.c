/*
 * it_keybag (iPhone 4S): the data-protection step of a restore, run by it_install from the
 * install boot. Formats the effaceable storage (the NAND's plog pool, AppleEffaceableNAND) and
 * creates the system keybag (/private/var/keybags/systembag.kb), the same two calls the fork's
 * contrib/it-keybag makes on the A4 boards and restored_external makes on a real restore:
 *
 *   AppleEffaceableStorage user client, selector 3 (isFormatted) and 4 (format);
 *   MKBKeyBagCreateSystem(NULL, "/mnt2") with the data volume mounted at /mnt2.
 *
 * Both are refused outside a trusted root (secure-root-prefix 'md': the restore ramdisk), which
 * is why this runs in the install boot and not from a job on the normal boot.
 *
 * Unlike it_install this program is linked against the ramdisk's own dyld: the IOKit and
 * MobileKeyBag calls are the IPSW's, found with dlopen/dlsym as it_keybag does, and the three
 * dl* entry points are the only imports (bound by dyld through the Mach-O's LC_DYLD_INFO, into the
 * slots below, from /usr/lib/libSystem.B.dylib). Everything else is the same no-libc style as
 * it_install: raw syscalls for the console, files and exit, so the host needs no iOS SDK
 * (tools/ios-prep/guest_install.py writes the Mach-O). The alternative, raw MIG to IOKit plus the
 * AppleKeyStore calls MKBKeyBagCreateSystem makes, would have meant re-implementing MobileKeyBag's
 * keybag creation (docs/n94/hardware.md "Storage: effaceable storage and the system keybag").
 *
 * Exit status: 0 with the keybag in place; 2 dyld/dlopen trouble, 3 effaceable storage not
 * formatted, 4 no keybag. Lines on the console start with "it_keybag: ".
 */
#include <stdint.h>

typedef uint32_t mach_port_t, io_object_t;
typedef int kern_return_t;

/* The imports, bound by dyld (guest_install.py finds them by name in the ELF's symbol table). */
__attribute__((section(".data.binds"))) void *(*volatile it_dlopen)(const char *, int);
__attribute__((section(".data.binds"))) void *(*volatile it_dlsym)(void *, const char *);
__attribute__((section(".data.binds"))) const char *(*volatile it_dlerror)(void);

#define RTLD_NOW 2

#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_sync 36
#define SYS_lseek 199
#define SYS_nanosleep 240
#define MACH_task_self (-28)

#define O_RDONLY 0
#define O_WRONLY 1
#define SEEK_END 2

#define DATA_MNT "/mnt2"
#define KEYBAG DATA_MNT "/keybags/systembag.kb"
#define IOKIT "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit"
#define MOBILEKEYBAG "/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag"

void *memset(void *d, int c, unsigned long n)
{
    uint8_t *p = d;
    while (n--) {
        *p++ = (uint8_t)c;
    }
    return d;
}

static long sys(long n, long a, long b, long c, long d)
{
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r12 __asm__("r12") = n;
    long carry;

    __asm__ volatile(
        "svc #0x80\n\t"
        "mov %[cy], #0\n\t"
        "movcs %[cy], #1\n\t"
        : "+r"(r0), [cy] "=&r"(carry)
        : "r"(r1), "r"(r2), "r"(r3), "r"(r12)
        : "memory", "cc");
    return carry ? -r0 : r0;
}

/* A Mach trap (negative number in r12) returns its value in r0, no carry. */
static long mach_trap(long n)
{
    register long r0 __asm__("r0") = 0;
    register long r12 __asm__("r12") = n;

    __asm__ volatile("svc #0x80" : "+r"(r0) : "r"(r12) : "memory", "cc");
    return r0;
}

static uint32_t slen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/*
 * Every message opens the console anew: the descriptor opened before dlopen stopped reaching the
 * console once the frameworks were loaded (the first install boot showed only the hello line), and
 * this keeps the log complete whatever libSystem's initializers do with the inherited descriptors.
 */
static void say(int con, const char *s)
{
    long fd = sys(SYS_open, (long)"/dev/console", O_WRONLY, 0, 0);

    (void)con;
    if (fd >= 0) {
        sys(SYS_write, fd, (long)s, slen(s), 0);
        sys(SYS_close, fd, 0, 0, 0);
    }
}

static void say_num(int con, const char *s, uint32_t v)
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do {
        buf[--i] = '0' + v % 10;
        v /= 10;
    } while (v);
    say(con, s);
    say(con, buf + i);
}

static void say_hex(int con, const char *s, uint32_t v)
{
    char buf[11];
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 8; i++) {
        buf[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    }
    buf[10] = 0;
    say(con, s);
    say(con, buf);
}

static void sleep_s(uint32_t s)
{
    uint32_t ts[2] = { s, 0 };
    sys(SYS_nanosleep, (long)ts, 0, 0, 0);
}

static __attribute__((noreturn)) void quit(int con, const char *why, int status)
{
    say(con, "it_keybag: ");
    say(con, why);
    say(con, "\n");
    sys(SYS_sync, 0, 0, 0, 0);
    sys(SYS_exit, status, 0, 0, 0);
    for (;;) {
    }
}

static void *need(int con, void *lib, const char *name)
{
    void *p = it_dlsym(lib, name);

    if (!p) {
        say(con, "it_keybag: dlsym ");
        say(con, name);
        say(con, ": ");
        say(con, it_dlerror());
        quit(con, "FAILED: symbol missing", 2);
    }
    return p;
}

/* The keybag file's size, or -1. */
static long file_size(const char *path)
{
    long fd = sys(SYS_open, (long)path, O_RDONLY, 0, 0);
    long size;

    if (fd < 0) {
        return -1;
    }
    size = sys(SYS_lseek, fd, 0, 0, SEEK_END);     /* fd, offset low, offset high, whence */
    sys(SYS_close, fd, 0, 0, 0);
    return size;
}

__attribute__((section(".text.start"), noreturn)) void _start(void)
{
    void *(*Matching)(const char *);
    io_object_t (*GetService)(mach_port_t, void *);
    kern_return_t (*Open)(io_object_t, mach_port_t, uint32_t, io_object_t *);
    kern_return_t (*Scalar)(io_object_t, uint32_t, const uint64_t *, uint32_t, uint64_t *, uint32_t *);
    int (*Create)(void *, const char *);
    void *iok, *mkb;
    io_object_t svc = 0, conn = 0;
    uint64_t fmt = 0;
    uint32_t n;
    kern_return_t kr, k3;
    long size;
    int con, i, r;

    con = (int)sys(SYS_open, (long)"/dev/console", O_WRONLY, 0, 0);
    say_num(con, "it_keybag: hello (dyld), console fd ", (uint32_t)con);
    say(con, "\n");
    sys(SYS_close, con, 0, 0, 0);

    iok = it_dlopen(IOKIT, RTLD_NOW);
    mkb = it_dlopen(MOBILEKEYBAG, RTLD_NOW);
    if (!iok || !mkb) {
        say(con, "it_keybag: dlopen: ");
        say(con, it_dlerror());
        quit(con, "FAILED: IOKit / MobileKeyBag not loaded", 2);
    }
    Matching = need(con, iok, "IOServiceMatching");
    GetService = need(con, iok, "IOServiceGetMatchingService");
    Open = need(con, iok, "IOServiceOpen");
    Scalar = need(con, iok, "IOConnectCallScalarMethod");
    Create = need(con, mkb, "MKBKeyBagCreateSystem");

    for (i = 0; i < 120 && !(svc = GetService(0, Matching("AppleEffaceableStorage"))); i++) {
        sleep_s(1);
    }
    if (!svc) {
        quit(con, "FAILED: no AppleEffaceableStorage", 3);
    }
    kr = Open(svc, (mach_port_t)mach_trap(MACH_task_self), 0, &conn);
    n = 1;
    k3 = kr ? kr : Scalar(conn, 3, 0, 0, &fmt, &n);
    say_hex(con, "it_keybag: effaceable open ", (uint32_t)kr);
    say_hex(con, " isFormatted ", (uint32_t)k3);
    say_num(con, " -> ", (uint32_t)fmt);
    say(con, "\n");
    if (!k3 && !fmt) {
        n = 0;
        say_hex(con, "it_keybag: format ", (uint32_t)Scalar(conn, 4, 0, 0, 0, &n));
        n = 1;
        k3 = Scalar(conn, 3, 0, 0, &fmt, &n);
        say_hex(con, " isFormatted ", (uint32_t)k3);
        say_num(con, " -> ", (uint32_t)fmt);
        say(con, "\n");
    }
    if (k3 || !fmt) {
        quit(con, "FAILED: effaceable storage not formatted", 3);
    }

    /* A second install boot keeps the keybag it has (MKBKeyBagCreateSystem refuses to replace one:
     * -4, "already exists"); the lockers it needs are in the effaceable storage just confirmed. */
    size = file_size(KEYBAG);
    if (size > 0) {
        say_num(con, "it_keybag: " KEYBAG " present, ", (uint32_t)size);
        say(con, " bytes\n");
        quit(con, "effaceable formatted, system keybag left as it is", 0);
    }
    r = Create(0, DATA_MNT);
    size = file_size(KEYBAG);
    say_num(con, "it_keybag: MKBKeyBagCreateSystem -> ", (uint32_t)r);
    say_num(con, ", " KEYBAG " ", (uint32_t)size);
    say(con, " bytes\n");
    sys(SYS_sync, 0, 0, 0, 0);
    if (r || size <= 0) {
        quit(con, "FAILED: no system keybag", 4);
    }
    quit(con, "effaceable formatted, system keybag created", 0);
}
