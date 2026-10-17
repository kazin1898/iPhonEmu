# iPhone 4S (n94ap, Apple A5 / S5L8940X) as iOS 6.1.3 describes it

Everything here comes from the 10B329 IPSW itself: `DeviceTree.n94ap.img3` (decrypted; the full dump
is `devicetree.txt`, the address map `iomap.tsv`) and the prelinked kexts of `kernelcache.release.n94`
(`kexts.txt`), plus the public A5 facts (n94linux, the Linux `apple-aic` driver). Rows marked **A4**
name the qemu-ios model for the S5L8930 block the A5 keeps; **new** is an A5 block with no model yet;
**hide** is a block the kboot device tree unmatches so the kernel does not wait on it.

Kernel: `Darwin Kernel Version 13.0.0 ... root:xnu-2107.7.55.2.2~1/RELEASE_ARM_S5L8940X`, armv7
(cputype 12 subtype 9), linked at 0x80001000, entry 0x800860c8. Its `pe_identify_machine` checks
**boot_args.Version == 18** (0x12) at 0x802794ca; `ios-prep` reads that off the kernel.

## Cores and memory

| Item | A5 (n94) | Notes |
|---|---|---|
| CPU | 2x `ARM,cortex-a9` (`cpus/cpu0`, `cpu1`) | cpu0 AIC lines {0xc0, 0x87, 0xc1}, cpu1 {0xc2, 0x8b, 0xc3}; `function-ipi_dispatch`, `function-enable_core` on both. Bring-up with `--cpus 1` (cpu1 unmatched). |
| L2 | PL310 at 0x3E000000 (+0x3FD00000) | **new**: QEMU's `l2x0` stub (`hw/misc/arm_l2x0.c`) serves the kernel's cache setup. |
| DRAM | 0x80000000, 512 MiB | `memory` reg is iBoot-filled. Kernel VA == PA at slide 0, so no KASLR slide is applied. vram (9,420,800 B) and pram (16 KiB) sit at the top, iBoot's layout. |
| SRAM / boot ROM | not in the DT | not needed for kboot. |
| Timebase | 24 MHz | `timebase-frequency`, `fixed-frequency` on cpu0 (iBoot-filled). |
| Timers | the AIC's per-CPU timer and 64-bit counter (below) | the PMGR is still `device_type timer` (its node is what `pe_arm_init_timer` maps) but the 10B329 kernel's rtclock never touches the A4 event timers at PMGR+0x2000: mach_absolute_time is AIC TIME_LO/HI and the deadline the AIC TMR_CNT, acked on FIQ. |
| Watchdog | 0x3F103020+0x10, AIC 4 | `wdt,s5l8940x|s5l8930x|s5l8920x` **A4** (inside the PMGR window). |

`arm-io` ranges: child 0x0 -> parent 0x30000000, size 0x10000000. Every `reg` below is absolute.
The A4's peripheral windows at 0x8xxxxxxx and 0xBFxxxxxx reappear at 0x3xxxxxxx and 0x3Fxxxxxx
with the same low bits: the display, DMA, USB, UART, SPI and PKE blocks keep their A4 offsets.

## Interrupts: the Apple AIC

| Item | A5 (n94) | Notes |
|---|---|---|
| AIC | 0x3F200000+0x10000, `aic,1`, `#main-cpus` 2 | `hw/arm/s5l8940_aic.c`. Replaces the A4's four PL192 VICs (0xBF200000). Register map confirmed against the `AppleInterruptController` kext (see "The AIC" below); the Linux `apple-aic` version-1 layout agrees. |
| GPIO | 0x3FA00000, AIC 119, `gpio,s5l8940x|gpio,s5l8930x`, 2 interrupt cells | **A4** `s5l8930_gpio.c` (the A4 GPIO is VIC 0x74). Buttons, PMU, touch, sensors and the radios interrupt through it. |

## The AIC, as the 10B329 kext drives it

Decoded from `AppleInterruptController` (kernelcache `__PRELINK_TEXT`, text at 0x80b060b0; `_aicVersion = 0`
since the DT has no `aic-version`, `_aicNumExtInts = 0xc0`, `_aicNumIPIs = 4`) and from the kernel's rtclock
seen through the model's trace (`-trace 's5l8940_aic*'`). Offsets from 0x3F200000:

| Offset | Register | What the kext does |
|---|---|---|
| 0x0004 | INFO | `[9:0]` external vectors (192), `[20:16]` CPUs - 1; the kext sizes its tables from it |
| 0x0010 | GLB_CFG | read, then written back with 0x70 ORed in (0x770 when `aic-version` != 0) at start |
| 0x0020 / 0x0028 | TIME_LO / TIME_HI | the kernel's timebase: read HI, LO, HI (24 MHz, the DT `timebase-frequency`) |
| 0x2000 | WHOAMI | the CPU reading; the IPI vector is `numExtInts + 2 * whoami (+1 for "other")` |
| 0x2004 | IACK | event: `[18:16]` type, 0 none (ends the handler's loop), 1 external vector in `[9:0]`, 4 IPI with `[1:0]` 1 = from another CPU, 2 = self; type 6 is `kAICIackVecTypeTimer`, which the kext panics on (the timer is FIQ, not IACK) |
| 0x2008 | IPI_SET | `ipi_dispatch` (vector 0xc0): bit 31 = self; `ipi_dispatch_other` (0xc1): bit n = CPU n |
| 0x200c | IPI_CLR | the handler clears the bit it took (1 or 0x80000000) |
| 0x2010..0x2020 | TMR_CFG, TMR_CNT, TMR_INT_STAT, TMR_STATE_SET/CLR | the current CPU's timer (the kernel's, below); `aic-version 0` propagates CPU 0's TMR_CFG to the others at start |
| 0x2024 / 0x2028 | IPI_MASK_SET / CLR | bit 0 "other", bit 31 "self" |
| 0x3000 + 4n | TARGET_CPU[n] | start writes 0 to all; a registered vector gets `(1 << #main-cpus) - 1` or its `target-destinations` entry (0x87 -> 1, 0x8b -> 2) |
| 0x4000 / 0x4080 + 4w | SW_SET / SW_CLR | software vectors (`ipid-mask` bits 0-3): set to trigger, cleared by the handler after dispatch |
| 0x4100 / 0x4180 + 4w | MASK_SET / MASK_CLR | start masks everything; `enableVector` clears, `disableVectorHard` sets; sleep saves the mask by reading 0x4180 |
| 0x4200 + 4w | HW_STATE | `callPlatformFunction("STAT")` reads a line's level |
| 0x5000 + 0x80 c + 0x10..0x28 | CPU c's bank | the 0x2010..0x2028 registers of any CPU: start masks every CPU's IPIs (0x5024 <- 0x80000001) and sleep/wake save and restore TMR_CFG and the IPI mask |

Acknowledging through IACK does **not** mask the vector: the handler never unmasks after dispatch, so a
level line that stays up is reported again on the next IACK (the model does the same). The kernel's
rtclock: at init TMR_CNT <- 0x7fffffff, TMR_CFG <- 1, TMR_INT_STAT <- 1; each deadline is a TMR_CNT write
(ticks from now, 24 MHz); the FIQ entry (0x80087b0c, reached through the FIQ-mode r9 the vector's
`mov pc, r9` uses) writes TMR_INT_STAT <- 1 and dispatches through `cpu_data + 0x30`. Two things the model
had to get right for the kernel to keep its clock (`hw/arm/s5l8940_aic.c` has the trace evidence):

- The timer **reloads** its count when it reaches zero: the kernel acks some FIQs without writing a new
  TMR_CNT and waits for the next one.
- The FIQ is **held until GLB_CFG has 0x70**: the kernel arms its first 9.5 ms deadline before the FIQ
  dispatcher has a callback (`cpu_data + 0x30` is NULL, the event is acked and dropped), and never re-arms
  after a dropped one, so a FIQ delivered before `AppleInterruptController::start` stops the clock for good.
  Which of the three bits gates it is unknown; the model uses all of them.

## The I2C controller (i2c,s5l8940x)

Not the A4's block: a PA Semi style FIFO master (Linux's `i2c-pasemi` serves the same design on later Apple
SoCs). Decoded from the 10B329 `AppleS5L8940XI2CController` (text at 0x807a5960; register accesses are the
virtuals +0x370 read / +0x374 write with the offset as an immediate):

| Offset | Register | Use |
|---|---|---|
| 0x00 | MTXFIFO | write a byte with 0x100 START (byte = `addr << 1 \| R/W`), 0x200 STOP, 0x400 READ (byte = count) |
| 0x04 | MRXFIFO | received bytes, 0x100 when empty |
| 0x08 | FIFO | `[7:0]` TX level, `[15:8]` RX level (only for the driver's progress timeout) |
| 0x0c | XFER | transfer state (printed in the panic) |
| 0x14 | SMSTA | W1C status: 0x10000000 XIP in progress, 0x08000000 XEN done, 0x00800000 and 0x40 errors, 0x00200000 MTN NAK, 0x00100000 RX FIFO not empty (live, never acked: the handler drains the FIFO) |
| 0x18 | IMASK | 0x08800040 for a write, 0x00b00040 for a read, 0 to quiet |
| 0x1c | CTL | clock divider; 0x200 written after a NAK |
| 0x2c | RXLEN | `count << 8` before a read |

A read of n bytes at register r: SMSTA <- 0x08800040, MTXFIFO <- `0x100 \| addr<<1`, `r`, `0x101 \| addr<<1`,
`0x600 \| n`, RXLEN <- n<<8, IMASK <- 0x00b00040; the interrupt handler reads SMSTA, drains MRXFIFO and acks.
An address NAK comes back as XEN with MTN (kIOReturn 0xe00002e9, `AppleDialogPMU: _readRegs failed`).

## What the kernel waits for: three handshakes found the hard way

- **`cpus=1`**: AppleARMPE sizes `IOCPUInterruptController` by counting every child of `/cpus`, hidden or
  not, lowered only by the `cpus=N` boot-arg; `IOCPUInterruptController::registerInterrupt` sleeps until that
  many CPUs are up, and `AppleInterruptController::start` registers its per-CPU lines through it. With 2
  expected on one core the AIC never registers as `IOInterruptController0E000000`, 19 drivers sleep in
  `lookUpInterruptController`, and no vector is ever unmasked (there was never a MASK_CLR write).
- **`SecureRoot`**: bsd_init's `IOSecureBSDRoot` calls AppleARMPE's `SecureRootName`, which records the root
  name (`md0` matches `secure-root-prefix` = `md`) and then sleeps until some kext calls `SecureRoot` and
  receives `SecureRootCallBack`. The caller is `IOAESAccelerator::start`, provided by `CDMAAES` on top of
  `AppleCDMA`: no CDMA model, no launchd. (IOFlashStorage, AppleEffaceableStorage and AppleKeyStore carry
  the string too.)
- **The IOPs**: `AppleA5IOP` and `A5AE2` start their EmbeddedIOP firmware (`s5l8940xiop-RELEASE`,
  `s5l8940xae2-RELEASE`, iBoot-1537.9.55) and wait for it; hidden until the IOP core model exists.

## Platform blocks

| Node | reg | AIC | compatible | Model |
|---|---|---|---|---|
| pmgr | 0x3F100000+0x7000; 0x38C00000, 0x38D00000, 0x38E00000, 0x38F00000, 0x3A500000, 0x3A600000 (+0x1000 each) | | `pmgr,s5l8940x` | **A4** `s5l8930_pmgr.c` with `gates` 0x78 (the A5 polls gate 0x44 at 0x1120 and pulses 0x5c; the A4 model's 0x40 gates read as zero there and the kernel spun forever) and `clkcfg-hi` (a second clock-config bank at 0x200-0x300 with the same busy bit). Still unmodeled: 0x38/0x3c, 0x1204, 0x2104/0x2108, 0x3100-0x3108. |
| dwi | 0x3F700000, AIC 6 | 6 | `dwi,s5l8940x` | **new** (`AppleS5L8940XDWI`): single-wire PMU buck control; the A4's SWI (0xBF600000) plays the same role. |
| iop | 0x3F300000 | 2 | `iop,s5l8940x`, `iop-version 1` | **new**: `AppleA5IOP` + `EmbeddedIOP-47.3` run `IOP_s5l8940xiop_firmware` (EmbeddedIOP iBoot-1537.9.55, 0x29000 bytes, Thumb-2). Not the A4 block: the control registers, the doorbells (AIC `IPID` functions) and the firmware's own use of the AIC are in "The IOP" below. A4 IOP is 0x86300000 + VICs at 0xBF300000. |
| ae2 | 0x341E0000, 0x341E1000, 0x341A0000, 0x341C1000, 0x34000000+0x30000 | 97, 98 | `ae2,s5l8940x` | **new / hide**: audio engine (`AppleA5AE2`, `IOP_s5l8940xae2_firmware`). |
| cdma | 0x37000000+0x2D000, 0x37800000+0x9000 (AES) | 51..94 | `cdma-version 3`, `max-channel 44` | **A4** `s5l8930_cdma.c` at the A5 bases (`dram-base`, `uart-base`, `paced-base` properties): channel n interrupts at 50+n; the model's 38 channels leave 39..44 in the unimplemented window. Its AES filter is what `CDMAAES`/`IOAESAccelerator` drive. |
| sha2 | 0x30200000 | 38 | `sha,s5l8940x` | **new**: SHA-256 (A4 has SHA-1 at 0x80100000, `s5l8930_sha1.c`). |
| pke | 0x33100000 | 17 | `pke-1,samsung` | **A4** `ipod_touch_pke.c` (0x83100000). |
| perfcounter, trace | 0x3F104000, 0x3F800000, 0x3D300000; 0x3D203000.. | | | unimplemented window. |

## Storage

| Node | reg | AIC | compatible | Model |
|---|---|---|---|---|
| flash-controller0 | FMI0 0x31200000, 0x31240000, 0x31280000; FMI1 0x31300000, 0x31340000, 0x31380000 | 33, 34 | `fmi,s5l8940x|fmi,s5l8920x` | **A4** `s5l8930_h2fmi.c` (0x81200000, VIC 0x22+n) behind the IOP (`AppleIOPFMI`). |
| flash-controller0/disk | | | `nand` | **PPN only**: `ppn-device`, `ppn-features`, `ppn-spec-version`, `ppn-ftl-ver 2`, `boot-from-nand 1`. The kernelcache carries `AppleSwissPPNFTL` (WhimoryPPN: PPNFPart, sVFL, sFTL) and **no raw-NAND FTL** (no `AppleNANDLegacyFTL`, no VSVFL/YaFTL/`NANDDRIVERSIGN` strings anywhere in it), so `ios-prep --nand raw` (the A4 geometry, `ppn-device 0`) can never give the kernel a root: the IOP firmware still has the raw h2fmi path (opcodes 1-12, 20, 21, the A4 model's contract) but nothing above `AppleIOPFMI` would open it. See "Storage: the decision". No `spi0`/NOR: NVRAM comes from `IOFlashNVRAM` on the NAND's `nvrm` partition (the `chosen/nvram-proxy-data` CHRP image only seeds it). |
| defaults | | | | `use-lwvm`, `content-protect`, `data-journaling`: LwVM partitions on the NAND, as the A4 6.x images. |

The expanded root filesystem is 1,279,774,720 bytes (HFSX, journaled), so an `rd=md0` RAM disk root
does not fit in 512 MiB: the disk has to come through the NAND model (or a trimmed root for M2 only).

## Buses and the devices on them

| Node | reg | AIC | compatible | Model |
|---|---|---|---|---|
| uart0 | 0x32500000 | 21 | `uart-1,samsung`, **boot-console** | **A4** (Samsung UART, `exynos4210` model as ipad1.c). The kernel's serial console. |
| uart1 | 0x32600000 | 22 | `uart-1,samsung`, `debug` | **A4** |
| uart3 | 0x32800000 | 24 | `uart-1,samsung`; child `bluetooth,n88` | **A4**; BT stub. |
| uart5 | 0x32A00000 | 26 | `uart-1,samsung`; child `gas-gauge,bq27540|gas-gauge,hdq` | **A4** `s5l8930_hdq.c` (the iPad's bq27545 model, 1420 mAh on n90). |
| spi1 | 0x32100000 | 29 | `spi-1,samsung`; child `multi-touch,n94` (GPIO irq 0x15) | **A4** `ipod_touch_spi.c`; touch: the A4 boards' multitouch models (N1F55 on n90). The n94 panel's profile is to find (`AppleMultitouchSPIC1F58`, `N1F55`, `N1F56` kexts; `multi-touch-calibration` 1 KiB). |
| i2c0 | 0x33200000 | 18 | `i2c,s5l8940x|iic,soft` | `hw/arm/s5l8940_i2c.c`: a PA Semi style FIFO master, not the A4 block ("The I2C controller" below). Devices: PMU `pmu,d1881` @0x74 (GPIO irq 0xd; `hw/arm/s5l8940_d1881.c`, a register file), codec `audio-control,cs42l63` @0x4a (the A4 CS42L58 model: the driver never checks the ID), `headset-switch,ts3a8235` @0x3b (none). |
| i2c1 | 0x33300000 | 19 | same | `compass,akm8975b` @0x0d (A4 AK8975 model), `als,tsl2581` @0x49 (A4 model). |
| i2c2 | 0x33400000 | 20 | same | `accelerometer,lis331dlh` @0x19 (the iPod LIS302DL model, WHO_AM_I 0x32; AppleLIS331DLH then fails for want of calibration data), `gyro,ap3gdl` @0x6a (A4 L3G4200D model; AppleAP3GDL finds ID 0xd3 and fails on a singular calibration matrix) and @0x68. |
| otgphyctrl | 0x36000000 | | `otgphyctrl,s5l8940x|s5l8930x` | **A4** `ipod_touch_usb_phys.c`, wired. |
| usb-complex | 0x3F108000; ranges -> 0x36100000+0x600000 | | `usb-complex,s5l8940x|s5l8930x` | **A4** models wired: usb-device 0x36100000 (AIC 11, the iPod Synopsys OTG with the A4 GHWCFG; `AppleSynopsysOTGDevice` finds 7+7 endpoints), usb-ehci 0x36400000 (AIC 12, `exynos4210-ehci`; its second window 0x36700000 stays unimplemented), usb-ohci0 0x36500000 (AIC 13), usb-ohci1 0x36600000 (AIC 14). USB_CTL at 0x3F108000 stays in the unimplemented window. |
| usb-ehci/wlan | HSIC port | GPIO 0x1f | | **new**: Wi-Fi is a BCM4330 over HSIC (`AppleBCMWLANBusInterfaceHSIC`), not the A4's SDIO card. There is no `sdio` node. |
| usb-ehci/hsic-baseband | HSIC port | GPIO 0x11 | | baseband over HSIC (`AppleBasebandN94`, `AppleBasebandUSB`); the DT `baseband` node is `baseband,n94`. **hide** at boot as the A4 boards do (`baseband=off`). |

## Display and media

| Node | reg | AIC | compatible | Model |
|---|---|---|---|---|
| clcd | 0x3A100000+0x7000 (display pipe), 0x39200000+0x2000 (CLCD), 0x39300000 | 43, 42 | `clcd,s5l8940x` | **A4** `s5l8930_display.c` (pipe 0x89000000, CLCD 0x89200000): same offsets. Kexts `AppleH4PDisplayPipe`, `AppleH4PCLCD`. 640x960 at scale 2. |
| mipi-dsim | 0x39500000, 0x3F110000 | 41 | `mipi-dsim-1,samsung`; child `lcd,pinot` | **A4** `ipod_touch_mipi_dsi.c` (0x89500000); `ApplePinotLCD`. |
| rgbout | 0x3A200000+0x7000, 0x39600000 | 44, 45 | `rgbout,s5l8940x` | **A4** (0x89100000 / 0x89600000). |
| tv-out | 0x39400000 | 40 | `tv-out,s5l8940x|s5l8930x` | **A4** (0x89400000). |
| displayport | 0x39700000, 0x34195000 | 46, 47 | `displayport,s5l8940x` | **new / hide** (`AppleSamsungDPTX`). |
| scaler (x2) | 0x38300000, 0x38400000 | 9, 10 | `scaler,s5l8940x|s5l8720x` | **A4** `ipod_touch_scaler.c` (one at 0x89300000). |
| jpeg | 0x38200000 | 118 | `jpeg,s5l8940x|s5l8920x` | **A4** `s5l8920_jpeg.c`. |
| vxd / vxe | 0x38100000, 0x38000000 (+0x100000) | 50, 48 | `vxd,s5l8940x|s5l8920x`, `vxe,s5l8940x` | **A4** `s5l8920_vxd.c` (decoder); encoder **hide**. |
| sgx | 0x35100000+0x18000, 0x3F10B000 | 49 | `sgx,s5l8940x` | SGX543MP2 (`IMGSGX543`): **hide** (`compatible none`), the GL bridge replaces it. |
| dart-nrt / dart-rt | 0x38B00000, 0x3A400000 (+0x2000) | 116, 117 | `dart,s5l8940x|s5l8930x` | **A4** `s5l8930_dart.c` (0x88D00000 / 0x89D00000). Mappers: jpeg, scaler; isp. |
| isp | 0x3A000000+0x100000 | 7, 8 | `isp,s5l8940x` | **A4** `s5l8930_isp.c`; **hide** by default (`camera=off`). |
| i2s0..3 | 0x34190000 + n*0x1000 | 99..102 | `i2s-1,samsung` | **A4** `s5l8930_i2s.c` (0x84500400 + n*0x1000, different base). i2s0 carries `audio-data,cs42l63`. |
| i2s-switch | 0x3FA01000 | | `i2s-switch,s5l8940x` | **new** (routing register, trivial). |
| highland-park | 0x3B020000, 0x3B010000, 0x3B000800, 0x3B000000 | 112..115 | `highland-park,s5l8940x` | **new / hide** (`HighlandParkAudioDevice`, baseband voice path). |

## Board-level nodes

`buttons` (GPIO irqs: menu, hold, volume, ringer), `dock` (`dock,30pin`, on the PMU's irq 5),
`charger` (`charger,n88`), `backlight`, `product` (`product-id` 20 bytes, filled from iBoot's
`DEVICEINFOBBT` block as ipad1.c does for n90), `chosen` (8 KiB `nvram-proxy-data`, `root-matching`,
`display-rotation`/`display-scale`, `boot-args` absent: boot_args carries the command line).

## The IOP (`AppleA5IOP`, `EmbeddedIOP-47.3`, `IOP_s5l8940xiop_firmware`)

Decoded 2026-10-17 from the 10B329 kexts (Ghidra, `tools/ghidra/DecompileByString.java` and friends; the
functions are named in `runs/n94/ghidra/strrefs.json`) and from the firmware image itself (`fwFirmwareImage`
section of the firmware kext, 0x29000 bytes, imported as ARM:LE:32:v7 at 0 with `IopFwAnalyze.java`).

- **Image**: an iBoot-1537 application like the A4's: ARM vector table at 0 with the handler addresses at
  0x20 (reset 0x40, undef 0xcb30, swi 0xcb68, pabt 0xcba0, dabt 0xcbdc, irq 0xcaa0, fiq 0xcae8), header words
  at 0x300 (`main` = 0xa7d, **Thumb**; link base 0; copy size 0x18000; bss 0x131c4-0x14000; stacks
  0x18800-0x19c00), the `cnfg` block at **0x14000**. The kext publishes `ConfigurationOffset 0x14000`,
  `HeapBaseOffset 0x1418c`, `HeapSizeOffset 0x14190`, `HeapRequired 0x20000`, `PageTableBase`/`PageTableSize`
  (new: the kext allocates a page table for the IOP), `MessageChannelSize 8`, `PanicString`, `PanicFunction`,
  `PanicLog`, `PanicLogBytes(Max)`. The ring table layout is EmbeddedIOP-20+'s (64-byte entries, the A4
  core's "v3"), which `hw/arm/s5l8930_iop.c`'s ring trace already reads.
- **Control block** (`AppleA5IOP::startCPU`, text 0x80d17924): the `iop` node's one register window
  (0x3F300000): `+0x00` <- 0 (stop), `+0x04` <- firmware physical base, `+0x34` <- 0x010001ff
  (0x030001ff with `iop-version` 1, which this DT has), `+0x00` <- 1 (run); stop writes `+0x00` <- 0. The
  kext also rounds the image length up to a power of two (>= 0x1000) but never writes it. The A4's cache
  controls, SELF and FW_SIZE registers are gone.
- **Doorbells**: the DT gives the node `function-doorbell {aic, 'IPID', 3}` and `function-nmi {aic, 'IPID',
  0x10002}`: both are AppleInterruptController platform functions, so the AP rings the IOP through the AIC
  (the kext's `ipid-mask {0xf}` names the four software vectors 0..3 the `SW_SET` word 0 can raise). The
  IOP answers on AIC vector 2 (`interrupts {2}`).
- **The firmware on the AIC**: it is a third AIC client. Its IRQ dispatcher (fw 0x2290) and FIQ dispatcher
  (0x2364) read **IACK at 0x3F202004** (the current-CPU bank, so WHOAMI must be its own number), take
  `0x1xxxx` as an external vector (`& 0x1ff`, then `MASK_CLR` word `v/32` bit `v%32` after the handler),
  **`0x70004` as its timer** (writes 1 to 0x3F202020 first, dispatches vector 0xc2) and **`0x70001` as the
  doorbell** (dispatches vector 0xc1, then writes 2 to 0x3F202020), and panic on `0x4xxxx`. Its timer is the
  per-CPU timer at 0x3F202010-0x3F20201c (`TIME_LO/HI` at 0x20/0x28 for the tick), as the kernel's; it also
  reads `GLB_CFG` 0x10. The PMGR it touches: 0x3F101024/28/2c/44, 0x3F1010cc (clock gates), 0x3F101200/1204.
  CDMA at 0x37001000+ (channel registers); the FMI through a base pointer (no absolute constants).
- **FMI task** (fw 0x1084, 512-byte commands as the A4, opcode at +0, status at +8, `is_ppn` per bus):
  raw opcodes 1-12, 20, 21 (the same table as the A4's EmbeddedIOP-20, served by `s5l8930_h2fmi.c` when the
  core runs), PPN opcodes 2-6, 10, 11, 13, 15-19, 21-26 (`h2fmi_ppn_iop_*`: read/write single/multiple,
  bootpage, erase, read_cau_bbt, get_controller_info, update_firmware, get_failure_info, set/get feature...).
  A PPN device's parameter page is validated by `h2fmi_ppn_get_device_params` (fw 0x76e4): caus 1..64,
  cau_bits 1..7, blocks/cau 0x400..0x2134, block_bits 10..15, pages/block 64..512 (== 1 << page_address_bits),
  SLC pages/block 64..256, bits/cell 1, page size 0x2020..0x4040, queue sizes 1..50, prep buffer <= 200.
  `#bootloader-bytes` is 0x600: boot pages are 1536 bytes.
- **Seen with the model** (`hw/arm/s5l8940_iop.c`, 2026-10-17): the kext runs the image in place (FW base
  0x80a8b000 is the firmware kext's `fwFirmwareImage` section in the kernelcache, `cnfg` at 0x80a9f000),
  the doorbell is **`SW_SET` word 0 bit 3** (0x3F204000 <- 8), the ring addresses are the AP's physical
  addresses **plus 0x40000000** (DRAM at 0xc0000000 in the IOP's map, as the A4's 'cnfg' memory map put
  it), rings 0 (control, 2 entries), 3 (sdio, 2), 5 and 6 (fmi0/1, 3 each). The handshake: 'nop ' on ring 0,
  then set_config (opcode 1) on each FMI ring, then the partition scheme's read_bootpage (opcode 11).
- **The AP side of PPN**: `AppleIOPFMI-89.3` reads 6-byte chip IDs per FMI:CE from the IOP, publishes
  `device-readid`, `ce-map`, `channels`, `#bootloader-bytes 0x600`, `dbg-chipids` on the nub and lets
  `IOFlashStorageDevice::initWithRegistryEntry` (IOFlashStorage-664.13.12) take the geometry from the DT
  (`#ce`, `#databus`, `toggle-device`, `#page-sectors`, `bbt-format`, `meta-per-logical-page`,
  `valid-meta-per-logical-page`, `logical-page-size`, `banks-per-ce`, `vendor-type`, `ecc-threshold`,
  `ecc-correctable`, `ppn-spec-version`, `blocks-cau`, `caus-ce`, `mlc-pages`, `match-oddeven-caus`,
  `max-transaction-size`, `#die-ce`): the chip ID has to agree with what iBoot would have written there, and
  `ios-prep` writes those properties.

## Storage: the PPN mailbox, the partition table and the FTL (B1-B2, 2026-10-17)

What `hw/arm/s5l8940_iop.c` serves and `tools/ios-prep/nand_ppn.py` writes, as found by driving the
kernel against the model (`runs/n94/m2-root.log`, `-trace 's5l8940_iop*'`):

- **Geometry** (`--nand ppn`, `ios_prep.py nand`): 2 channels x 1 chip, 2 CAUs x 2048 blocks x 128 pages
  (64 SLC) x 8 KiB + 32 bytes meta (16 per 4 KiB logical page), chip ID `98 3a 94 93 76 5a`, PPN 1.0,
  `max-transaction-size 64` (the FTL's pages per command; at 0 its page-run loop never adds a page and
  `s_vfl.c:0xfba`'s read/write loop spins forever). Row = cau << 18 | block << 7 | page; bit 19 is the
  SLC-mode flag the partition scheme's pool partitions are addressed with.
- **Commands** (512 bytes, opcode +0, status +8, args from +0x18): 1 set_config (+0x88 is_ppn), 3/4/5/6
  erase/read/raw/write single (u16 CE, block or row, data buffer, meta buffer), 10/11 write/read bootpage
  (CE, row, 1536-byte buffer), 13 read CAU BBT (through a PPN_COMMAND, the bitmap through the data list),
  16/17/18 read/erase/write multi (PPN_COMMAND), 22 controller info (6 feature strings + the device
  parameters page), 23 die IDs, 24 power state, 25 temperature, 26 debug data. CEs are named FMI1:CE8 style.
- **PPN_COMMAND** (AppleSwissPPNFTL's, at +0x2c, length 0x1c + 12 per page): +8 u16 pages, +0xa u16 logical
  pages, +0xc the OR of the statuses, +0xe/+0x14 chip records {CE, pages, first entry}, entries at +0x1c
  {u16 bytes, u16 column, u32 row, u8 chip record, u8 logical pages, u8 status}; data and meta go through
  the command's segment lists in logical-page units. Statuses the sFTL accepts: 0x40 done, 0x49 done and
  clean, 0x42-0x45, 0x51, 0xc1; 0x48 is "unidentified" and panics the FTL open.
- **The partition table** (boot pages 0 and 1 of block 0 of every chip, `nand_ppn.build_ptab`): header
  'ndrG', major 1, minor 9; geometry at 0x200; cemetery, spares and substitutions with Fletcher-16 guards;
  32 entries at 0x400 from the kernel's own PPN format string (boot slice, plog/nvrm/firm pools, the fsys
  bulk, the info entries); entry names are the integer 'boot' stored little-endian. With it the kernel
  publishes Boot Block, Effaceable, NVRAM, Firmware, Filesystem, System Config, Diagnostic Data and Bad
  Block Table.
- **The first boot** (`--add-boot-arg nand-enable-reformat=1`, no ramdisk): AppleSwissPPNFTL finds no
  signature and formats (VFL, FPart special blocks 0xC105/0xC104, FTL, signature 0xC101: about 4100
  erases, 3000 writes), then opens: `FTL_Open [OK]`, a block device of 1953125 sectors of 4 KiB;
  IOFlashNVRAM wipes and formats the nvrm pool ("nand nvram started successfully"), AppleEffaceableNAND
  finds no content. The store keeps all of it (sparse XOR-0xff files, 7 MB on disk after the format), so
  the next boot opens the FTL directly. The kernel then waits for IOMedia partition 1 on that disk: B3.
- **NVRAM on the NAND** (`IOFlashNVRAM`, `nand_nvram_core.c`): the 8 KiB image is a 0x5a `nvram` CHRP
  header (adler32 of the rest at +0x10, a generation at +0x14) followed by the 0x70 `common` partition
  (2 KiB) and a 0x7f filler; each sync writes it as a new generation of six 1536-byte boot pages (a
  10-byte page header: u32 generation, u8 pages - 1, u8 index, u16 3, u16 0xbead) into the nvrm pool's
  blocks, one copy per chip (CAU 1 blocks 2042/2043 in the kernel's own table), and the next start scans
  the copies six pages at a time for the newest generation. What kept it from surviving a boot was the
  kernel's side, not the store: `IODTNVRAM::initProxyData` takes `chosen/nvram-proxy-data` as the whole
  image and `registerNVRAMController` then never reads the controller, so every boot synced the seed
  `ios-prep` used to put there (a `common`/`free` image with no 0x5a header), and `_validateAppleHeader`
  found "bad signature" and wiped the pool again. With `--nand ppn` the bundle now carries no proxy (the
  device tree loses the property: an all-zero placeholder would be walked as endless empty partitions)
  and the kernel reads the image from IOFlashNVRAM, as xnu does on any board without a proxy.
- The answer interrupt is deferred 100 us after the doorbell, as the A4 HLE does; erases of blocks that
  are already holes are skipped (QEMU's win32 file driver has no unmap, so zeroing would allocate).
- **Marker pages** (B3): `markers.json` beside `geometry.json` names host files ("sources"); a stored
  logical page whose first 16 bytes are the marker magic (`iPhonEmu-marker1`, then u32 source, u32 4 KiB
  block, u32 source ^ block ^ 0xA5000000) is served as that block of that file, on the PPN read-multi and
  single-read paths. The guest's installer writes the system volume as 312,445 such pages, so the FTL lays
  the volume out in its own (undocumented) on-flash format while the host keeps the 1.2 GiB; a page the
  guest rewrites loses the magic and is an ordinary page from then on. The store grows by the markers
  themselves (1.3 GB on disk after an install: each is a 4 KiB page of mostly zeros, stored XOR 0xff).

## Storage: the install boot (B3, 2026-10-17)

The system volume reaches the FTL's disk through the guest, as a restore would put it there, with the
IPSW's own restore ramdisk as the trusted root and the emulator's helper where `restored_external` runs:

- **`contrib/it-install/it_install.c`**, the 4S's counterpart of the fork's `it_keybag`/`it_boot`: no libc,
  Darwin syscalls through `svc 0x80` (number in r12, carry set on error). `tools/ios-prep/guest_install.py`
  builds it with the MSYS2 clang as a bare-metal armv7 ELF (ARM mode, no PIC, no writable data), links it
  with `ld.lld` at the Mach-O's `__TEXT` address and wraps the one segment in a static Mach-O
  (`MH_EXECUTE`, `__PAGEZERO`, `__TEXT`, `LC_UNIXTHREAD`, no dyld, no signature: the boot-args carry the
  fork's `amfi_allow_any_signature=1` and `cs_enforcement_disable=1`). The kernel execs it from the
  ramdisk grown by `hfs.py` (the stock image is 100% full) and it runs as pid 3 under launchd.
- **The ARM kernel's syscall arguments** are packed 32-bit words (APCS): its own `lseek` handler reads
  `fd, offset low, offset high, whence` from r0-r3 (`sysent` at 0x802f00b8: `pread` has 5 words,
  `lseek` 4, `mmap` 7, no munge functions), so a 64-bit argument is two consecutive words with no
  alignment pad. `pread`/`pwrite` on `/dev/rdisk0` still returned EOF and `EIO` wherever the offset was
  put, while `lseek` + `read`/`write` on the raw device and `pread`/`pwrite` on the buffered `/dev/disk0`
  work; the helper uses `lseek` + `read`/`write` (its `IT_PROBE` build keeps the diagnostics). `fork`
  returns the pid to both sides and tells the child by r1 = 1 (libc's stub zeroes r0 there).
- **What the helper does**: waits for `/dev/rdisk0` (4096-byte blocks, 1,953,125 of them), writes the
  system partition as marker pages in 128 KiB writes (159,770 PPN page programs through the model, 40 s of
  wall time), then the MBR (two `0xAF` entries: system at LBA 1, data to the end), closes the disk so
  `IOFDiskPartitionScheme` publishes `disk0s1`/`disk0s2`, runs the ramdisk's `newfs_hfs -s -v Data`,
  `fsck_hfs -fy` and `mount_hfs` on the data partition, creates the directories iOS expects under
  `/private/var`, execs `it_keybag` (B4, below), writes launchd's overrides database
  (`db/launchd.db/com.apple.launchd/overrides.plist`, what `launchctl unload -w` writes) with the Bluetooth
  jobs disabled (and keybagd, only if `it_keybag` failed), syncs and `reboot(RB_HALT | RB_QUICK)`. Every step is idempotent; a second run finds the MBR and only revisits the
  data volume.
- **Why keybagd was held off** (until B4, below): with no system keybag (`/private/var/keybags/systembag.kb`,
  which needs formatted effaceable storage and MobileKeyBag) `keybagd`'s `kb_load` fails, it prints `FATAL
  KEYBAG ERROR: kb_load`, sets `auto-boot=false` and reboots into recovery 20 s after launchd starts. The
  knob lives on the emulator's data volume; the system volume stays as Apple shipped it.
- **The halt**: `reboot(RB_HALT)` runs the kernel's shutdown (`AppleNANDFTL::systemWillShutdown`, the FTL
  flushed) and `pmu go stdby`, after which the kernel panics (`panic(cpu 0 caller 0x80017d65)`): the D1881
  model has no standby state. Harmless for the install boot (the run script quits on `it_install: done`);
  the model should power the machine off there.

## Storage: effaceable storage and the system keybag (B4, 2026-10-17)

What SpringBoard needs and launchd does not: formatted effaceable storage (the NAND's `plog` pool,
`AppleEffaceableNAND`) and the system keybag (`/private/var/keybags/systembag.kb`, whose class keys
securityd, lockdownd and SpringBoard unwrap through AppleKeyStore). Both are made by `restored_external`
on a real restore and both are refused outside a trusted root (`format attempt from untrusted root`; the
restore ramdisk is one, `secure-root-prefix` 'md'), so they are a step of the install boot:

- **The model needed nothing new.** `[effaceable:ERR] unable to find content` on every boot was the
  unformatted state, not a missing page. The format erased the pool's SLC blocks through the single-page
  erase (opcode 3, rows `0xbfe00`/`0xffe00`/`0xffd80` on both chips: the plog pool lives in the spare
  blocks at the end of CAU 1, SLC flag set), wrote its content and the kernel printed `[effaceable:INIT]
  formatting storage`, `succeeded`, `format succeeded`; every boot since reads it back as `found current
  generation, 2, in group 1` (single-page reads of the same rows, the blank pages answered 0xffffffff).
- **The helper, and how it is linked.** `contrib/it-install/it_keybag.c` makes the two calls the fork's
  `contrib/it-keybag` makes on the A4 boards: `AppleEffaceableStorage` user client selector 3
  (isFormatted) and 4 (format), then `MKBKeyBagCreateSystem(NULL, "/mnt2")` with the data volume mounted
  at `/mnt2`. Two ways were open for a helper with no iOS SDK on the host: (a) raw MIG to IOKit
  (`io_service_get_matching_service`, `io_service_open_extended`, `io_connect_method`) plus everything
  `MKBKeyBagCreateSystem` does on top of AppleKeyStore (the bag's creation, its wrapping, the effaceable
  locker, the file's format), which would have meant re-implementing MobileKeyBag from its disassembly;
  (b) a dyld-linked helper that calls the ramdisk's own IOKit and MobileKeyBag. (b) was chosen and cost
  one build change: `guest_install.py` compiles `it_keybag.c` with the same bare-metal clang as
  `it_install.c`, links it with a data segment on the page after the code (`.data.binds`: the three
  `dlopen`/`dlsym`/`dlerror` pointer slots) and writes a Mach-O with `__TEXT`, `__DATA`, `__LINKEDIT`,
  `LC_DYLD_INFO_ONLY` whose bind opcodes make dyld fill the slots from `/usr/lib/libSystem.B.dylib`,
  `LC_LOAD_DYLINKER /usr/lib/dyld`, `LC_LOAD_DYLIB`, an empty `LC_SYMTAB`/`LC_DYSYMTAB` and
  `LC_UNIXTHREAD` (dyld-212.3.2 jumps to that entry with the kernel's stack once libSystem is initialized;
  not PIE, so the bare-metal code's absolute addresses hold). The rest of the helper is the same no-libc
  style as `it_install` (raw syscalls, `mach_task_self` as trap -28). No code signature and no
  entitlements: the IPSW's own `restored_external` carries none either (its signature has no entitlements
  blob; keybagd's `com.apple.keystore.device` is for the normal boot), and the kernel grants the system
  bag from the secure root: `booted from secure root: give device keybag access to everyone`.
- **Where it sits.** `hfs.py` only replaces an existing file's data fork, so the helper takes the ramdisk's
  `/usr/bin/TiSerialFlasher` (the touch-controller flasher, of no use in the emulator); `it_install` execs
  it after the data volume is mounted and populated, with the child's stdout/stderr on the console (so
  dyld's own complaints would show), and writes launchd's overrides afterwards: keybagd is disabled only
  when the helper fails. A second install boot finds the storage formatted and the keybag present
  (`MKBKeyBagCreateSystem` refuses to replace one: -4, `already exists`) and leaves both.
- **The console descriptor.** The helper's first build printed only its hello line: the `/dev/console`
  descriptor opened before `dlopen` no longer reached the console once the frameworks were loaded (the
  program's exit status was still 0). The helper now opens the console for every message.
- **The run** (`runs/n94/m2-install.log`): `it_keybag: hello (dyld), console fd 5`, `effaceable open 0x0
  isFormatted 0x0 -> 0`, `format 0x0`, `isFormatted -> 1`, `MKBKeyBagCreateSystem -> 0,
  /mnt2/keybags/systembag.kb 1495 bytes`, 8 s from power-on to `it_install: done` on a store that already
  has the system volume.

## Storage: the decision (2026-10-17)

What the IPSW says, in order of consequence:

1. The 10B329 n94 kernelcache has **one FTL, `AppleSwissPPNFTL`** (WhimoryPPN/Core: PPNFPart, SVFL `s_vfl.c`,
   SFTL `s_*.c` with an L2V tree, WhimoryBoot's signature). There is no raw-NAND FTL at all, so the plan's
   path (a) - raw NAND through the A4 h2fmi model and a K48NAND-style image - cannot work here however well
   the IOP firmware's raw path runs. The board is **boot-from-nand** (`chosen/boot-from-nand 1`): the NAND
   holds `IOFlashPartitionScheme`'s table in its boot blocks, with the PPN format string
   `slice:boot:1 pool:plog:4:1 pool:nvrm:2:3 flex:firm:2:s:/:firmware-min-capacity bulk:fsys info:scfg
   info:diag info:fbbt` (boot block, effaceable, NVRAM, firmware, the FTL's bulk, and the info specials), and
   the kernel refuses to format that table outside a trusted root (`partition table format not allowed in
   this context`; the restore ramdisk's `restored_external`/`ioflashstoragetool` do it).
2. `IOFlashNVRAM` formats an erased `nvrm` pool by itself ("starting with freshly wiped NVRAM") and publishes
   the `IONVRAM` restored waits for (it keeps its content across boots once the kernel is not fed a proxy
   image, see "NVRAM on the NAND" above); `AppleEffaceableNAND` needs a format from a trusted root, as on the A4
   (docs/ipad1/ios4.md "The blocker"), and the system keybag needs that.
3. `AppleSwissPPNFTL` formats an unformatted bulk partition itself when the boot-args carry
   `nand-enable-reformat=1` (`_DestructiveFormat`: FPart, VFL, FTL, then the signature); without it: "Unformatted
   media requires nand-enable-reformat boot-arg". Its on-flash format (sVFL contexts, sFTL superblocks, BTOCs,
   the L2V) is undocumented anywhere public; the fork's A4 generator (LightTouchMac `K48NAND.swift`) only knows
   VSVFL/YaFTL.
4. The restore ramdisk (9.9 MB, `ramdisk-restore.dmg`) carries `restored_external`, `asr`, `newfs_hfs`,
   `mount_hfs`, `fdisk`, `nvram`, `ioflashstoragetool`, `launchd`, MobileKeyBag - and no shell.

Decision (the user's, 2026-10-17, option B below): **PPN, with the IOP modeled at the mailbox** (the
A4 HLE's shape, `s5l8930_iop.c`), **and the guest doing its own install** instead of an offline sFTL image:

| Step | What | Reuses | Cost |
|---|---|---|---|
| B1 | The IOP as an AIC client (its own WHOAMI, IACK type-7 timer/doorbell events, MASK_CLR, TARGET bit), the 0x3F300000 block, the `IPID` doorbell/NMI, vector 2 back, EmbeddedIOP-47 rings (64-byte entries, `cnfg` at 0x14000) and control messages | `s5l8930_iop.c`, `s5l8940_aic.c` | 2-3 days |
| B2 | The PPN FMI opcodes on a PPN page store (CAU/block/page geometry from the DT, parameter page, controller info, CAU BBT, 1536-byte boot pages, read/program/erase multi with PPN status arrays) | the raw opcode handlers as a template; the firmware's `h2fmi_ppn_iop_*` and the kext's `generatePpnDmaLists` give the structs | 5-8 days |
| B3 | **done 2026-10-17** ("Storage: the install boot" above): the partition table from `ios-prep`, the FTL's own format on the first boot, then the system volume as marker pages written by `contrib/it-install` from the restore ramdisk and served from `rootfs.hfs` by the page store; `BSD root: disk0s1`, launchd and the daemons from the NAND | `s5l8930_iop.c`'s store, `tools/ios-prep` | 4-6 days |
| B4 | **done 2026-10-17** ("Storage: effaceable storage and the system keybag" above): the effaceable format and the system keybag from the install boot, by a dyld-linked helper calling the ramdisk's IOKit and MobileKeyBag (no iOS toolchain needed); keybagd back on | ipad1's one-shot, `contrib/it-keybag` | 3-5 days, after M2 |

About three weeks to the M2 definition (real root from the NAND, IONVRAM, launchd and daemons, no reboot loop).
Alternatives, costed:

- **A. The IOP core running the real firmware** (the plan's preferred shape) plus a model of the A5 FMI's
  **sequencer** (`fmiss_ppn.c`: SEQ_MACRO_CONTROL, COMMAND/OPERAND/STORE FIFOs, SEQ_INT_PND) and the PPN wire
  protocol under it: B1 as a real AIC port + 2-3 more weeks for the sequencer and PPN hardware, same B3/B4.
  Authentic (and the ae2 would follow), but the sequencer is undocumented and only the firmware describes it.
- **C. A RAM-disk root** (`rd=md0`) with a trimmed 6.1.3 root (<= ~450 MB, fonts/dictionaries/carrier bundles
  out) on a 1 GiB machine: 2-3 days to launchd and daemons from the real files, but no NAND, no persistence,
  IONVRAM still missing; a demo, not M2.
- **D. A donor kernelcache** (iPad2,1 10B329, the same S5L8940X kernel) if it carries `AppleNANDLegacyFTL`:
  would reopen path (a) with the proven A4 generator, at the price of a kernelcache not built for this board
  (PMU, panel, touch and baseband kexts differ); an hour to check, outcome uncertain.

## Open points for the machine model

1. ~~AIC register map~~: confirmed against the kext (section above). Still open in the model: which GLB_CFG
   bit gates the timer FIQ, whether the timer reload is a true auto-reload or a count that keeps running,
   the per-CPU timer on cpu1 (`0x5090 <- 1` at start, no FIQ line yet), and what the kernel expects of an
   IPI it sends itself (0x2008 <- 0x80000000 while the IPI vector is still masked; with the clock running it
   moves on).
2. **PMGR timer / clock table**: the A5's `clock-frequencies` index map; `ios-prep` writes the A4 map. The
   A5 PMGR has registers the A4 model does not know and the kernel reads back as zero without complaint so
   far: clock configs 0x220-0x254 and 0x2f0-0x2f8, 0x3c, gates 0x1110-0x115c and 0x1180, 0x1204, 0x2108
   (<- 0x51), 0x3100-0x3108. The wdt at +0x3020 (A4: +0x2020) has not been touched by 10B329 yet.
3. **Unmodeled blocks the kernel writes at boot** (reads come back 0, nothing complains): CDMA 0x37000008
   and 0x3700000c (one bit per channel, all 44), usb-complex 0x3F108000, i2s-switch 0x3FA01000-0x3FA0103C
   (all 0), the PL310 "cif" window 0x3FD01020 (<- 1) and 0x3FD01120 (<- 0x80000100). `AppleH4CamIn::start`
   (isp) and `AppleSamsungDPTXControllerS5L8940X` start without faulting on the unimplemented window.
4. **PMU**: `hw/arm/s5l8940_d1881.c` models what AppleD1881PMU (text 0x80aea268, decompiled with the
   Ghidra project) is seen to use: EVENT_A..G at 0x01-0x07 (read 7 at once by the interrupt handler,
   clear on read; EVENT_B bit 0x20 = ADC done), IRQ_MASK_A..G at 0x0e-0x14 (1 = masked), the ADC (0x43
   control with 0x20 = start, 12-bit result as 0x44[3:0] | 0x45 << 4, mid-scale for every channel until
   they are identified), the RTC counter at 0x5d-0x60 (32-bit seconds, the host's clock; the RTC class adds
   the offset it keeps at 0xa4) and the restart (0x96 <- 0x2b after 0xa1 <- 0, from the kernel's `pmu
   restart`, resets the machine). Its interrupt is GPIO 0x0d, active low. The driver's `ADC read bad state`
   and the 1970 date are gone; which ADC channel is what (battery, NTCs, dock) is still open, as are the
   wake events and buttons.
   **USB**: wired (table above); restored's `enable_usb_connections` still fails and it waits for the
   `IONVRAM` resource, which on this NOR-less board comes from the NAND's partitions.
5. ~~I2C~~: decoded and modeled (section above).
6. **Storage**: decided by the user on 2026-10-17 ("Storage: the decision", option B) and done through B3
   the same day: PPN through a mailbox-level IOP model and a PPN page store, the guest formatting the NAND
   and installing the system volume on its first boot, and B4 (effaceable storage and the system keybag,
   keybagd running) the same day. Open: the data volume's journal (`newfs_hfs -J`), the PMU standby on halt.
7. **Wi-Fi over HSIC**: an HSIC BCM4330 is a new device; a stub (device absent) suffices until M4.
8. **SMP**: `function-enable_core` / `ipi_dispatch` on `cpus`: what the kernel pokes to start cpu1.
