/*
 * it_install: the emulator's installation helper for the iPhone 4S (n94) machine, the guest half
 * of the first boot that puts the system volume on the emulated NAND. It is to the 4S what
 * contrib/it-keybag and contrib/it-boot are to the A4 boards: a small program of the emulator's
 * own that runs once inside the guest where the stock restore flow would run restored.
 *
 * The 4S kernel only has AppleSwissPPNFTL for its NAND, and the FTL's on-flash layout is its own
 * business, so the host cannot write a volume into the page store offline. Instead, the IPSW's own
 * restore ramdisk is booted once with this program staged as /usr/local/bin/restored_external (the
 * ramdisk's /etc/rc.boot execs it, where a real restore hands over to the restore daemon; the same
 * place the fork's it_keybag takes on the A4 boards). It does what restored's partitioning step
 * does, with the volume data coming from the host:
 *
 *   1. opens /dev/console and /dev/rdisk0 (the FTL's block device, 4 KiB sectors);
 *   2. fills the system partition with *marker* pages: one 4 KiB page each, naming the host file
 *      and 4 KiB block that holds its real bytes (MARKER_MAGIC, source, block, check). The page
 *      store (hw/arm/s5l8940_iop.c) serves such a page from that host file, so the FTL holds the
 *      whole volume in its own layout while the host keeps the bytes; pages the guest rewrites
 *      later are ordinary pages;
 *   3. writes an MBR with two Apple_HFS partitions: the system volume and the data volume (the
 *      MBR last, so a half-done install is not taken for a finished one);
 *   4. runs the ramdisk's own newfs_hfs on the data partition, as restored would, mounts it at
 *      /mnt2 and creates the directories iOS expects under /private/var;
 *   5. execs it_keybag (contrib/it-install/it_keybag.c, dyld-linked, staged on the ramdisk by
 *      guest_install.py), the data-protection step: it formats the effaceable storage and
 *      creates the system keybag at /mnt2/keybags/systembag.kb, as restored_external does on a
 *      real restore and the fork's it_keybag on the A4 boards;
 *   6. writes launchd's overrides database (db/launchd.db/com.apple.launchd/overrides.plist)
 *      disabling the Bluetooth jobs (no controller behind uart3, as on the fork's A4 boards) and,
 *      only if step 5 failed, keybagd (without the system keybag its kb_load fails and it reboots
 *      the device into recovery 20 s after launchd). The system volume stays as Apple shipped it;
 *      the knobs live on the emulator's data volume;
 *   7. syncs and halts.
 *
 * Every step is idempotent: a second run finds the MBR ("already installed") and only repairs a
 * data partition with no volume. Progress goes to the console as "it_install: ..." lines; the end is
 * "it_install: done" (or "it_install: FAIL <why>", after which it parks so the log can be read).
 *
 * Built by tools/ios-prep/guest_install.py with the MSYS2 clang (bare-metal armv7 ELF object, lld,
 * then a static Mach-O wrapper written in Python): no libc, Darwin's syscalls through svc 0x80
 * (number in r12, result in r0, carry set on an error). The install table and sizes come from
 * it_install_table.h, generated per store. No .data or .bss: the program is one read-only,
 * executable segment, so every state lives on the stack.
 */
#include <stdint.h>
#include "it_install_table.h"

#define BLOCK 4096u
#define CHUNK 32u                      /* marker pages per write (128 KiB on the stack) */

/* Darwin syscall numbers (bsd/kern/syscalls.master, iOS 6). */
#define SYS_exit 1
#define SYS_fork 2
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_wait4 7
#define SYS_chown 16
#define SYS_sync 36
#define SYS_ioctl 54
#define SYS_reboot 55
#define SYS_execve 59
#define SYS_chmod 15
#define SYS_dup2 90
#define SYS_fsync 95
#define SYS_mkdir 136
#define SYS_pread 153                  /* only the IT_PROBE build uses pread/pwrite */
#define SYS_pwrite 154
#define SYS_lseek 199
#define SYS_unmount 159
#define SYS_nanosleep 240

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define RB_HALT 0x08
#define RB_QUICK 0x400
#define DKIOCGETBLOCKSIZE 0x40046418u
#define DKIOCGETBLOCKCOUNT 0x40086419u
#define DKIOCSYNCHRONIZECACHE 0x20006416u

#define DATA_DEV "/dev/rdisk0s2"
#define DATA_BDEV "/dev/disk0s2"
#define DATA_MNT "/mnt2"
#define KEYBAG_HELPER "/usr/bin/TiSerialFlasher"   /* it_keybag's slot on the ramdisk (guest_install.py) */

typedef long ssize_t;

/* The compiler may still open-code these names; give it the bodies (no libc here). */
void *memset(void *d, int c, unsigned long n)
{
    uint8_t *p = d;
    while (n--) {
        *p++ = (uint8_t)c;
    }
    return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
    uint8_t *p = d;
    const uint8_t *q = s;
    while (n--) {
        *p++ = *q++;
    }
    return d;
}

/* Four register arguments and up to two on the stack, as the kernel's ARM munge expects them. */
static long sys6(long n, long a, long b, long c, long d, long e, long f, int *err)
{
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r12 __asm__("r12") = n;
    long carry;

    __asm__ volatile(
        "sub sp, sp, #16\n\t"
        "str %[e], [sp]\n\t"
        "str %[f], [sp, #4]\n\t"
        "svc #0x80\n\t"
        "add sp, sp, #16\n\t"
        "mov %[cy], #0\n\t"
        "movcs %[cy], #1\n\t"
        : "+r"(r0), [cy] "=&r"(carry)
        : "r"(r1), "r"(r2), "r"(r3), "r"(r12), [e] "r"(e), [f] "r"(f)
        : "memory", "cc");
    *err = (int)carry;
    return r0;
}

static long sys(long n, long a, long b, long c, long d)
{
    int err;
    long r = sys6(n, a, b, c, d, 0, 0, &err);
    return err ? -r : r;
}

/*
 * Positioned I/O on the block device: lseek(2) then read/write. The ARM kernel takes a syscall's
 * arguments as packed 32-bit words (APCS, as its own lseek handler reads them: fd, offset low,
 * offset high, whence), so a 64-bit offset is two consecutive words with no alignment pad. pread
 * and pwrite on the raw device returned EOF and EIO whatever the offset's placement (the
 * IT_PROBE build below records that), while lseek + read/write and the buffered /dev/disk0 work.
 */
#define SEEK_SET 0

static long lseek64(int fd, uint64_t off)
{
    return sys(SYS_lseek, fd, (long)(uint32_t)off, (long)(uint32_t)(off >> 32), SEEK_SET);
}

static ssize_t xfer(int write, int fd, void *buf, uint32_t len, uint64_t off)
{
    if (lseek64(fd, off) < 0) {
        return -1;
    }
    return sys(write ? SYS_write : SYS_read, fd, (long)buf, len, 0);
}


#ifdef IT_PROBE
/* pread/pwrite with the offset in r3 + first stack word (layout 1) or in the stack pair (layout 0). */
static ssize_t prw_with(int layout, long n, int fd, void *buf, uint32_t len, uint64_t off)
{
    int err;
    long lo = (long)(uint32_t)off, hi = (long)(uint32_t)(off >> 32);
    long r = layout ? sys6(n, fd, (long)buf, len, lo, hi, 0, &err)
                    : sys6(n, fd, (long)buf, len, 0, lo, hi, &err);
    return err ? -r : r;
}
#endif

/*
 * fork(2) raw: Darwin returns the pid in r0 to both sides and tells them apart in r1 (1 in
 * the child, 0 in the parent); libc's stub turns the child's r0 into 0, done here instead.
 */
static long sys_fork(int *child)
{
    register long r0 __asm__("r0");
    register long r1 __asm__("r1");
    register long r12 __asm__("r12") = SYS_fork;
    long carry;

    __asm__ volatile(
        "svc #0x80\n\t"
        "mov %[cy], #0\n\t"
        "movcs %[cy], #1\n\t"
        : "=r"(r0), "=r"(r1), [cy] "=&r"(carry)
        : "r"(r12)
        : "memory", "cc");
    if (carry) {
        return -r0;
    }
    *child = r1 != 0;
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

static void say(int con, const char *s)
{
    if (con >= 0) {
        sys(SYS_write, con, (long)s, slen(s), 0);
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
    say(con, "\n");
}

static void msleep(uint32_t ms)
{
    uint32_t ts[2] = { ms / 1000, (ms % 1000) * 1000000 };
    sys(SYS_nanosleep, (long)ts, 0, 0, 0);
}

static void park(int con, const char *why)
{
    say(con, "it_install: FAIL ");
    say(con, why);
    say(con, "\n");
    sys(SYS_sync, 0, 0, 0, 0);
    for (;;) {
        msleep(10000);
    }
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static void marker(uint8_t *page, uint32_t source, uint32_t block)
{
    memset(page, 0, BLOCK);
    memcpy(page, MARKER_MAGIC, 16);
    put32(page + 16, source);
    put32(page + 20, block);
    put32(page + 24, source ^ block ^ MARKER_CHECK);
}

static int wait_open(const char *path, int flags, uint32_t tries)
{
    for (;;) {
        long fd = sys(SYS_open, (long)path, flags, 0, 0);
        if (fd >= 0) {
            return (int)fd;
        }
        if (!tries--) {
            return -1;
        }
        msleep(500);
    }
}

static void mbr(uint8_t *sector, uint64_t disk_blocks)
{
    /* Two Apple_HFS (0xAF) entries, LBAs in 4 KiB sectors: system, then data to the end. */
    uint32_t p1_start = SYSTEM_LBA, p1_count = SYSTEM_BLOCKS;
    uint32_t p2_start = p1_start + p1_count;
    uint32_t p2_count = (uint32_t)(disk_blocks - p2_start);
    uint8_t *e = sector + 446;

    memset(sector, 0, 512);
    e[0] = 0x00; e[1] = 0xfe; e[2] = 0xff; e[3] = 0xff; e[4] = 0xaf; e[5] = 0xfe; e[6] = 0xff; e[7] = 0xff;
    put32(e + 8, p1_start);
    put32(e + 12, p1_count);
    e += 16;
    e[0] = 0x00; e[1] = 0xfe; e[2] = 0xff; e[3] = 0xff; e[4] = 0xaf; e[5] = 0xfe; e[6] = 0xff; e[7] = 0xff;
    put32(e + 8, p2_start);
    put32(e + 12, p2_count);
    sector[510] = 0x55;
    sector[511] = 0xaa;
}

/* fork, exec and wait; the child's exit status, or -1. */
static int run(int con, const char *path, const char *const argv[])
{
    static const char *const envp[] = { 0 };
    int child = 0, status = -1;
    long pid = sys_fork(&child);

    if (pid < 0) {
        return -1;
    }
    if (child) {
        /* the child's stdout and stderr go to the console, so the tools' messages reach the log */
        long c = sys(SYS_open, (long)"/dev/console", O_RDWR, 0, 0);

        if (c >= 0) {
            sys(SYS_dup2, c, 1, 0, 0);
            sys(SYS_dup2, c, 2, 0, 0);
        }
        sys(SYS_execve, (long)path, (long)argv, (long)envp, 0);
        sys(SYS_exit, 127, 0, 0, 0);
    }
    while (sys(SYS_wait4, pid, (long)&status, 0, 0) < 0) {
        msleep(100);
    }
    say(con, "it_install: ran ");
    say(con, path);
    say_num(con, " -> status ", (uint32_t)status);
    return status;
}

/* Does the data partition hold an HFS+ volume (the signature at byte 1024 of the partition)? */
static int data_has_volume(uint8_t *buf)
{
    int fd = wait_open(DATA_DEV, O_RDONLY, 120);
    int yes;

    if (fd < 0) {
        return -1;
    }
    yes = xfer(0, fd, buf, BLOCK, 0) == BLOCK && buf[1024] == 'H' && (buf[1025] == '+' || buf[1025] == 'X');
    sys(SYS_close, fd, 0, 0, 0);
    return yes;
}

static void mkdir_as(const char *path, int mode, int uid)
{
    sys(SYS_mkdir, (long)path, mode, 0, 0);
    sys(SYS_chmod, (long)path, mode, 0, 0);
    if (uid) {
        sys(SYS_chown, (long)path, uid, uid, 0);
    }
}

#define O_CREAT 0x200
#define O_TRUNC 0x400

/*
 * launchd's overrides (what `launchctl unload -w` writes): BTServer and BlueTool off as the fork keeps
 * them on the A4 boards (docs/ipad1/README.md "Bluetooth: parked"): there is no Bluetooth controller
 * behind uart3, and BlueTool's HCI traffic through AppleOnboardSerial's rx DMA path ended in a kernel
 * assertion (AppleSimpleUARTSync.cpp:2177). keybagd off only when it_keybag left no system keybag.
 */
static const char overrides_head[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>com.apple.BTServer</key>\n\t<dict>\n\t\t<key>Disabled</key>\n\t\t<true/>\n\t</dict>\n"
    "\t<key>com.apple.BlueTool</key>\n\t<dict>\n\t\t<key>Disabled</key>\n\t\t<true/>\n\t</dict>\n";
static const char overrides_keybagd[] =
    "\t<key>com.apple.mobile.keybagd</key>\n\t<dict>\n\t\t<key>Disabled</key>\n\t\t<true/>\n\t</dict>\n";
static const char overrides_tail[] = "</dict>\n</plist>\n";

static int write_overrides(const char *path, int keybagd_off)
{
    long fd = sys(SYS_open, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644, 0);
    long n = 0;

    if (fd < 0) {
        return -1;
    }
    n += sys(SYS_write, fd, (long)overrides_head, sizeof(overrides_head) - 1, 0);
    if (keybagd_off) {
        n += sys(SYS_write, fd, (long)overrides_keybagd, sizeof(overrides_keybagd) - 1, 0);
    }
    n += sys(SYS_write, fd, (long)overrides_tail, sizeof(overrides_tail) - 1, 0);
    sys(SYS_close, fd, 0, 0, 0);
    return n == (long)(sizeof(overrides_head) + sizeof(overrides_tail) - 2 +
                       (keybagd_off ? sizeof(overrides_keybagd) - 1 : 0)) ? 0 : -1;
}

/* The data volume gets the directories launchd and the daemons expect (restored's skeleton) and the
 * overrides database; every step is idempotent, so this runs on every install boot. */
static void populate_data(int con)
{
    static const char *const fsck[] = { "fsck_hfs", "-fy", DATA_DEV, 0 };
    static const char *const mount[] = { "mount_hfs", DATA_BDEV, DATA_MNT, 0 };
    static const char *const newfs[] = { "newfs_hfs", "-s", "-J", "-v", "Data", DATA_DEV, 0 };
    static const char *const keybag[] = { "it_keybag", 0 };
    static const char *const dirs[] = {
        "/db", "/db/timezone", "/empty", "/folders", "/keybags", "/log", "/log/asl", "/logs", "/logs/CrashReporter",
        "/msgs", "/preferences", "/preferences/SystemConfiguration", "/root", "/root/Library", "/run",
        "/spool", "/vm", "/wireless", "/wireless/Library", "/wireless/Library/Preferences", "/Keychains",
        "/MobileDevice", "/MobileDevice/ProvisioningProfiles", "/Managed Preferences",
        "/Managed Preferences/mobile", "/audit", "/ea", "/mobile", "/mobile/Library", "/mobile/Media",
        "/mobile/Applications", "/mobile/Documents", "/mobile/Library/Preferences",
        "/mobile/Library/Caches", "/mobile/Library/Logs", "/mobile/Library/Keyboard",
        "/db/launchd.db", "/db/launchd.db/com.apple.launchd", 0
    };
    char path[96];
    uint32_t i, j, k;
    int kb;

    run(con, "/sbin/fsck_hfs", fsck);        /* a volume the last boot left mounted needs its journal replayed */
    if (run(con, "/sbin/mount_hfs", mount) != 0) {
        /* Past repair (an emulator quit mid-write before the volume was journaled): made anew, as a
         * restore would make it; the keybag step below then creates a new system keybag for it. */
        say(con, "it_install: mount_hfs failed, newfs_hfs Data again\n");
        if (run(con, "/sbin/newfs_hfs", newfs) != 0 || run(con, "/sbin/mount_hfs", mount) != 0) {
            say(con, "it_install: data volume left empty\n");
            return;
        }
    }
    for (i = 0; dirs[i]; i++) {
        const char *d = dirs[i];
        int mode = 0755, uid = 0;

        for (j = 0; DATA_MNT[j]; j++) {
            path[j] = DATA_MNT[j];
        }
        for (k = 0; d[k] && j + 1 < sizeof(path); k++) {
            path[j++] = d[k];
        }
        path[j] = 0;
        if (d[1] == 'm' && d[2] == 'o') {
            uid = 501;                      /* mobile */
        } else if (d[1] == 'w' && d[2] == 'i') {
            uid = 25;                       /* _wireless */
        } else if (d[1] == 't' || (d[1] == 'r' && d[2] == 'u') || d[1] == 'e') {
            mode = 01777;
        }
        mkdir_as(path, mode, uid);
    }
    mkdir_as(DATA_MNT "/tmp", 01777, 0);
    /* The data-protection step, with the data volume mounted: the effaceable format needs the trusted
     * root this ramdisk is, and the keybag goes to DATA_MNT/keybags. Its stdout/stderr reach the console
     * (dyld's own complaints included). */
    kb = run(con, KEYBAG_HELPER, keybag);
    say(con, kb == 0 ? "it_install: system keybag in place, keybagd stays enabled\n"
                     : "it_install: no system keybag (it_keybag failed), keybagd disabled\n");
    say_num(con, "it_install: launchd overrides (BTServer, BlueTool disabled) -> ",
            (uint32_t)write_overrides(DATA_MNT "/db/launchd.db/com.apple.launchd/overrides.plist", kb != 0));
    sys(SYS_sync, 0, 0, 0, 0);
    say_num(con, "it_install: data volume populated, unmount -> ",
            (uint32_t)sys(SYS_unmount, (long)DATA_MNT, 0, 0, 0));
}

__attribute__((section(".text.start"), noreturn)) void _start(void)
{
    /* Journaled (-J): the emulator is quit without a guest shutdown, and the journal is what keeps the
     * volume mountable after that (an unjournaled one went past fsck's repair once). */
    static const char *const newfs[] = { "newfs_hfs", "-s", "-J", "-v", "Data", DATA_DEV, 0 };
    uint8_t buf[CHUNK * BLOCK] __attribute__((aligned(16)));
    uint64_t blocks = 0;
    uint32_t bsize = 0, i, n, chunk;
    int disk, st, con, has;

    con = wait_open("/dev/console", O_WRONLY, 20);
    say(con, "it_install: hello (" MARKER_MAGIC ")\n");

    disk = wait_open("/dev/rdisk0", O_RDWR, 240);
    if (disk < 0) {
        park(con, "no /dev/rdisk0");
    }
    if (sys(SYS_ioctl, disk, DKIOCGETBLOCKSIZE, (long)&bsize, 0) < 0 ||
        sys(SYS_ioctl, disk, DKIOCGETBLOCKCOUNT, (long)&blocks, 0) < 0) {
        park(con, "DKIOCGETBLOCK*");
    }
    say_num(con, "it_install: block size ", bsize);
    say_num(con, "it_install: blocks ", (uint32_t)blocks);
#ifdef IT_PROBE
    /* Diagnostics of the block device's syscall surface (build with IT_INSTALL_CFLAGS=-DIT_PROBE). */
    {
        int k, ok;

        say_num(con, "probe: pread(rdisk0) 4K at 0, offset in r3+[sp] -> ", (uint32_t)prw_with(1, SYS_pread, disk, buf, BLOCK, 0));
        say_num(con, "probe: pread(rdisk0) 4K at 0, offset in [sp],[sp+4] -> ", (uint32_t)prw_with(0, SYS_pread, disk, buf, BLOCK, 0));
        say_num(con, "probe: lseek 9 GiB -> ", (uint32_t)lseek64(disk, 9ull << 30));
        say_num(con, "probe: read 4K there -> ", (uint32_t)sys(SYS_read, disk, (long)buf, BLOCK, 0));
        say_num(con, "probe: lseek 7 GiB -> ", (uint32_t)lseek64(disk, 7ull << 30));
        say_num(con, "probe: read 4K there -> ", (uint32_t)sys(SYS_read, disk, (long)buf, BLOCK, 0));
        marker(buf, 7, 7);
        say_num(con, "probe: write 4K at 4096 -> ", (uint32_t)xfer(1, disk, buf, BLOCK, BLOCK));
        memset(buf, 0, BLOCK);
        say_num(con, "probe: read 4K at 4096 -> ", (uint32_t)xfer(0, disk, buf, BLOCK, BLOCK));
        say_num(con, "probe:   magic ok, block 7 -> ", buf[0] == 'i' && buf[15] == '1' && buf[20] == 7);
        for (k = 0; k < (int)CHUNK; k++) {
            marker(buf + k * BLOCK, 1, 1000 + k);
        }
        say_num(con, "probe: write 128K at 1 GiB -> ", (uint32_t)xfer(1, disk, buf, CHUNK * BLOCK, 1ull << 30));
        memset(buf, 0, CHUNK * BLOCK);
        say_num(con, "probe: read 128K at 1 GiB -> ", (uint32_t)xfer(0, disk, buf, CHUNK * BLOCK, 1ull << 30));
        ok = 1;
        for (k = 0; k < (int)CHUNK; k++) {
            uint8_t *pg = buf + k * BLOCK;
            if (pg[0] != 'i' || pg[15] != '1' || pg[20] != (uint8_t)(1000 + k) || pg[21] != (uint8_t)((1000 + k) >> 8)) {
                ok = 0;
            }
        }
        say_num(con, "probe:   all 32 markers ok -> ", ok);
        memset(buf, 0, BLOCK);
        say_num(con, "probe: read 4K at 0 -> ", (uint32_t)xfer(0, disk, buf, BLOCK, 0));
        say_num(con, "probe:   first byte ", buf[0]);
        park(con, "probe done");
    }
#endif
    if (bsize != BLOCK || blocks < SYSTEM_LBA + SYSTEM_BLOCKS + 1024) {
        park(con, "disk geometry");
    }

    /* Already installed (an MBR with our system partition)? Then only the data volume is checked. */
    if (xfer(0, disk, buf, BLOCK, 0) == BLOCK && buf[510] == 0x55 && buf[511] == 0xaa &&
        buf[446 + 4] == 0xaf) {
        say(con, "it_install: already installed\n");
        sys(SYS_close, disk, 0, 0, 0);
        goto data;
    }

    /* How much the block device takes per write: the largest power of two up to CHUNK that works. */
    for (chunk = CHUNK; chunk; chunk >>= 1) {
        for (i = 0; i < chunk; i++) {
            marker(buf + i * BLOCK, SYSTEM_SOURCE, i);
        }
        if (xfer(1, disk, buf, chunk * BLOCK, (uint64_t)SYSTEM_LBA * BLOCK) == (ssize_t)(chunk * BLOCK)) {
            break;
        }
        say_num(con, "it_install: write refused at pages ", chunk);
    }
    if (!chunk) {
        say_num(con, "it_install: 32-page read -> ", (uint32_t)xfer(0, disk, buf, CHUNK * BLOCK, 0));
        park(con, "no write size works");
    }
    say_num(con, "it_install: pages per write ", chunk);
    say_num(con, "it_install: writing markers for source 0, pages ", SYSTEM_BLOCKS);
    for (i = 0; i < SYSTEM_BLOCKS; i += n) {
        uint32_t k;

        n = SYSTEM_BLOCKS - i < chunk ? SYSTEM_BLOCKS - i : chunk;
        for (k = 0; k < n; k++) {
            marker(buf + k * BLOCK, SYSTEM_SOURCE, i + k);
        }
        if (xfer(1, disk, buf, n * BLOCK, (uint64_t)(SYSTEM_LBA + i) * BLOCK) != (ssize_t)(n * BLOCK)) {
            park(con, "marker write");
        }
        if ((i & 0x3fff) == 0) {
            say_num(con, "it_install: at ", i);
        }
    }
    say(con, "it_install: markers written\n");

    /* The MBR last, so a half-done install is not taken for a finished one. */
    memset(buf, 0, BLOCK);
    mbr(buf, blocks);
    if (xfer(1, disk, buf, BLOCK, 0) != BLOCK) {
        park(con, "MBR write");
    }
    sys(SYS_fsync, disk, 0, 0, 0);
    sys(SYS_ioctl, disk, DKIOCSYNCHRONIZECACHE, 0, 0);
    sys(SYS_close, disk, 0, 0, 0);      /* closing the whole disk after a write re-probes its partitions */
    say(con, "it_install: MBR written\n");

data:
    has = data_has_volume(buf);
    if (has < 0) {
        park(con, "no " DATA_DEV);
    }
    if (has) {
        say(con, "it_install: data volume present\n");
    } else {
        say(con, "it_install: newfs_hfs Data\n");
        st = run(con, "/sbin/newfs_hfs", newfs);
        if (st != 0) {
            park(con, "newfs_hfs");
        }
    }
    populate_data(con);
    sys(SYS_sync, 0, 0, 0, 0);
    msleep(1000);
    say(con, "it_install: done\n");
    sys(SYS_sync, 0, 0, 0, 0);
    msleep(1000);
    sys(SYS_reboot, RB_HALT | RB_QUICK, 0, 0, 0);
    for (;;) {
        msleep(10000);
    }
}
