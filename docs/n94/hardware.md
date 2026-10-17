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
| iop | 0x3F300000 | 2 | `iop,s5l8940x` | **A4** `s5l8930_iop.c` + `s5l8930_iop_core.c` run the IOP firmware (`IOP_s5l8940xiop_firmware` in the kernelcache). A4 IOP is 0x86300000 + VICs at 0xBF300000. |
| ae2 | 0x341E0000, 0x341E1000, 0x341A0000, 0x341C1000, 0x34000000+0x30000 | 97, 98 | `ae2,s5l8940x` | **new / hide**: audio engine (`AppleA5AE2`, `IOP_s5l8940xae2_firmware`). |
| cdma | 0x37000000+0x2D000, 0x37800000+0x9000 (AES) | 51..94 | `cdma-version 3`, `max-channel 44` | **A4** `s5l8930_cdma.c` at the A5 bases (`dram-base`, `uart-base`, `paced-base` properties): channel n interrupts at 50+n; the model's 38 channels leave 39..44 in the unimplemented window. Its AES filter is what `CDMAAES`/`IOAESAccelerator` drive. |
| sha2 | 0x30200000 | 38 | `sha,s5l8940x` | **new**: SHA-256 (A4 has SHA-1 at 0x80100000, `s5l8930_sha1.c`). |
| pke | 0x33100000 | 17 | `pke-1,samsung` | **A4** `ipod_touch_pke.c` (0x83100000). |
| perfcounter, trace | 0x3F104000, 0x3F800000, 0x3D300000; 0x3D203000.. | | | unimplemented window. |

## Storage

| Node | reg | AIC | compatible | Model |
|---|---|---|---|---|
| flash-controller0 | FMI0 0x31200000, 0x31240000, 0x31280000; FMI1 0x31300000, 0x31340000, 0x31380000 | 33, 34 | `fmi,s5l8940x|fmi,s5l8920x` | **A4** `s5l8930_h2fmi.c` (0x81200000, VIC 0x22+n) behind the IOP (`AppleIOPFMI`). |
| flash-controller0/disk | | | `nand` | **PPN**: `ppn-device`, `ppn-features`, `ppn-spec-version`, `ppn-ftl-ver`; `AppleSwissPPNFTL` is in the kernelcache, and so is the raw-NAND `AppleNANDFTL`. `ios-prep --nand raw` (default) writes the A4 raw geometry and `ppn-device 0` so the FTL takes the h2fmi path; whether 6.1.3's FMI firmware accepts a raw device on the A5 is the first storage question. There is no `spi0`/NOR: NVRAM comes as `chosen/nvram-proxy-data` (filled with an empty CHRP image) and `IOFlashNVRAM`. |
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
| otgphyctrl | 0x36000000 | | `otgphyctrl,s5l8940x|s5l8930x` | **A4** (0x86000000). |
| usb-complex | 0x3F108000; ranges -> 0x36100000+0x600000 | | `usb-complex,s5l8940x|s5l8930x` | **A4**: usb-device 0x36100000 (AIC 11, Synopsys OTG), usb-ehci 0x36400000 + 0x36700000 (AIC 12), usb-ohci0 0x36500000 (AIC 13), usb-ohci1 0x36600000 (AIC 14). |
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
4. **PMU**: `hw/arm/s5l8940_d1881.c` is a register file; AppleD1881PMU starts on it (reads 0x5d x4 at
   boot, then ADC: `_readADCGated(3) ADC read bad state: 1`), `getGMTTimeOfDay` still has no RTC. The
   registers that matter (events, RTC, ADC, buttons) are to be given meaning from the kext.
   **USB**: the restore ramdisk's restored reboots after `enable_usb_connections failed`, and the reboot
   path panics in `AppleUSBEHCI::UIMInitialize` on the unimplemented EHCI (0x36400000); the Synopsys OTG
   device controller (0x36100000) also starts and finds zeros. The A4 models fit both.
5. ~~I2C~~: decoded and modeled (section above).
6. **Storage** (M2: the kernel sits at `Still waiting for root device`): raw NAND through the IOP's FMI
   firmware vs a PPN model.
7. **Wi-Fi over HSIC**: an HSIC BCM4330 is a new device; a stub (device absent) suffices until M4.
8. **SMP**: `function-enable_core` / `ipi_dispatch` on `cpus`: what the kernel pokes to start cpu1.
