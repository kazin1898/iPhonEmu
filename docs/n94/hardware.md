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
| Watchdog | 0x3F103020+0x10, AIC 4 | `wdt,s5l8940x|s5l8930x|s5l8920x`: the A4 block at the A5's offset (the PMGR model's `a5`), driven by `AppleS5L8930XWatchDogTimer`; "Platform" below. |

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
  TMR_CNT and waits for the next one. The reload is at least 1 ms (24,000 ticks): when the count left
  was a deadline a few ticks away, reloading it as is fired the FIQ about 100,000 times a second of host
  time, the CPU sat in the FIQ handler (half the PC samples in FIQ mode, the rest in
  AppleInterruptController) and the UI froze minutes into Setup (M4). A count the kernel writes is exact.
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
  `s5l8940xae2-RELEASE`, iBoot-1537.9.55) and wait for it: both are modeled high-level now (`s5l8940_iop.c` B1, `s5l8940_ae2.c` M3).

## Platform blocks

| Node | reg | AIC | compatible | Model |
|---|---|---|---|---|
| pmgr | 0x3F100000+0x7000; 0x38C00000, 0x38D00000, 0x38E00000, 0x38F00000, 0x3A500000, 0x3A600000 (+0x1000 each) | | `pmgr,s5l8940x` | **A4** `s5l8930_pmgr.c` with `gates` 0x78 (the A5 polls gate 0x44 at 0x1120 and pulses 0x5c; the A4 model's 0x40 gates read as zero there and the kernel spun forever) and `clkcfg-hi` (a second clock-config bank at 0x200-0x300 with the same busy bit); `cores` and `a5` add the A5's core control, performance and power registers and the watchdog's address ("SMP", "Platform"). |
| dwi | 0x3F700000, AIC 6 | 6 | `dwi,s5l8940x` | `AppleS5L8940XDWI`: single-wire PMU buck control (the A4's SWI at 0xBF600000 plays the same role); a register file in `n94.c` ("Platform"). |
| iop | 0x3F300000 | 2 | `iop,s5l8940x`, `iop-version 1` | **new**: `AppleA5IOP` + `EmbeddedIOP-47.3` run `IOP_s5l8940xiop_firmware` (EmbeddedIOP iBoot-1537.9.55, 0x29000 bytes, Thumb-2). Not the A4 block: the control registers, the doorbells (AIC `IPID` functions) and the firmware's own use of the AIC are in "The IOP" below. A4 IOP is 0x86300000 + VICs at 0xBF300000. |
| ae2 | 0x341E0000, 0x341E1000, 0x341A0000, 0x341C1000, 0x34000000+0x30000 | 97, 98 | `ae2,s5l8940x` | **new** `s5l8940_ae2.c` (high-level, M3): audio engine (`AppleA5AE2`, `IOP_s5l8940xae2_firmware`); section "The AE2". |
| cdma | 0x37000000+0x2D000, 0x37800000+0x9000 (AES) | 51..94 | `cdma-version 3`, `max-channel 44` | **A4** `s5l8930_cdma.c` at the A5 bases (`dram-base`, `uart-base`, `paced-base` properties): channel n interrupts at 50+n; the model's 38 channels leave 39..44 in the unimplemented window (no boot reaches them). Its AES filter is what `CDMAAES`/`IOAESAccelerator` drive (SecureRoot, the keybag, content protection); no `CDMA::` error in a boot. |
| sha2 | 0x30200000 | 38 | `sha,s5l8940x` | **unused by 10B329**: no kext in the kernelcache matches `sha,s5l8940x` (the only SHA class is IOCryptoAcceleratorFamily's generic `IOSHA1Accelerator`, with no provider; corecrypto hashes in software), and a boot never touches 0x30200000. Left in the unimplemented window. |
| pke | 0x33100000 | 17 | `pke-1,samsung` | **A4** `ipod_touch_pke.c` (0x83100000), matched by `AppleSamsungPKE`; nothing calls on it during a boot and Setup (`IT_PKE_DEBUG=1` logs no access). |
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
| uart5 | 0x32A00000 | 26 | `uart-1,samsung`; child `gas-gauge,bq27540|gas-gauge,hdq` | **A4** `s5l8930_hdq.c`, wired since 2026-10-19: 1432 mAh, DEVICE_TYPE 0x0540, the machine's `battery-level` ("PMU and sensors"). |
| spi1 | 0x32100000 | 29 | `spi-1,samsung`; child `multi-touch,n94` (GPIO irq 0x15) | **A4** `ipod_touch_spi.c` (64 KiB TX FIFO for the firmware burst) with the N1 digitizer model `ipod_touch_multitouch.c`, profile `mt_profile_n94` (M4, "Touch and buttons" below). |
| i2c0 | 0x33200000 | 18 | `i2c,s5l8940x|iic,soft` | `hw/arm/s5l8940_i2c.c`: a PA Semi style FIFO master, not the A4 block ("The I2C controller" below). Devices: PMU `pmu,d1881` @0x74 (GPIO irq 0xd; `hw/arm/s5l8940_d1881.c`, a register file), codec `audio-control,cs42l63` @0x4a (the A4 CS42L58 model: the driver never checks the ID), `headset-switch,ts3a8235` @0x3b (none). |
| i2c1 | 0x33300000 | 19 | same | `compass,akm8975b` @0x0d (A4 AK8975 model), `als,tsl2581` @0x49 (A4 model). |
| i2c2 | 0x33400000 | 20 | same | `accelerometer,lis331dlh` @0x19 (the iPod LIS302DL model, WHO_AM_I 0x32), `gyro,ap3gdl` @0x6a (A4 L3G4200D model, ID 0xd3) and @0x68 (the DT's alternative address, empty as on a unit with its gyro at 0x6a). Both start since the kboot DT carries a nominal calibration ("PMU and sensors"). |
| otgphyctrl | 0x36000000 | | `otgphyctrl,s5l8940x|s5l8930x` | **A4** `ipod_touch_usb_phys.c`, wired. |
| usb-complex | 0x3F108000; ranges -> 0x36100000+0x600000 | | `usb-complex,s5l8940x|s5l8930x` | **A4** models wired: usb-device 0x36100000 (AIC 11, the iPod Synopsys OTG with the A4 GHWCFG; `AppleSynopsysOTGDevice` finds 7+7 endpoints; the host reaches it through `tools/dev/usbmux_bridge.py`, "USB: the host" below), usb-ehci 0x36400000 (AIC 12, `exynos4210-ehci`; its second window 0x36700000 stays unimplemented), usb-ohci0 0x36500000 (AIC 13), usb-ohci1 0x36600000 (AIC 14). USB_CTL at 0x3F108000 stays in the unimplemented window. |
| usb-ehci/wlan | HSIC port | GPIO 0x1f | | **new**: Wi-Fi is a BCM4330 over HSIC (`AppleBCMWLANBusInterfaceHSIC`), not the A4's SDIO card. There is no `sdio` node. Absent: its DEVICE_READY (GPIO 0x307) rests low and the port stays empty; the driver resets the chip three times and gives up. |
| usb-ehci/hsic-baseband | HSIC port | GPIO 0x11 | | baseband over HSIC (`AppleBasebandN94`, `AppleBasebandUSB`); the DT `baseband` node is `baseband,n94`. Absent: DEVICE_READY (GPIO 0x201) rests low, the port stays empty (`device has not connected after 1500ms`, then the port powers down). |

## Display and media

| Node | reg | AIC | compatible | Model |
|---|---|---|---|---|
| clcd | 0x3A100000+0x7000 (display pipe), 0x39200000+0x2000 (CLCD), 0x39300000 | 43, 42 | `clcd,s5l8940x` | **A4** `s5l8930_display.c` (pipe 0x89000000, CLCD 0x89200000): same offsets. Kexts `AppleH4PDisplayPipe`, `AppleH4PCLCD`. 640x960 at scale 2. |
| mipi-dsim | 0x39500000, 0x3F110000 | 41 | `mipi-dsim-1,samsung`; child `lcd,pinot` | **A4** `ipod_touch_mipi_dsi.c` (0x89500000); `ApplePinotLCD`. reg[1] is the panel clock's slow adaptive clocking block (`sac-version 1`, the `sacm` node's `saca` function 'lcd0'): `n94.c` models it as a register file whose +0x00 reads bit 8 (ready) set ("Touch and buttons" below). |
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
  flushed) and `pmu go stdby`, after which the kernel panicked (`panic(cpu 0 caller 0x80017d65)`) while the
  D1881 model had no standby state. Since 2026-10-19 the halt powers the machine off and QEMU exits ("Power:
  off, restart, and a session cut short").

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

## Storage: the FMI's inline AES (2026-10-18)

Why the units forgot Setup on every boot, showed no wallpaper and drew the Calendar icon with a strip or no
page: the files behind them (`com.apple.purplebuddy.plist`, SpringBoard's `IconState.plist` and
`LockBackground.cpbitmap`, the icon caches) are content protected, and the page store did not apply the
AES the kernel asks the FMI for. Their catalog records were on the NAND, their data was not readable.

- **The command** (the IOP's PPN read and program, opcodes 16 and 18, the 512-byte command at `cmd`):
  +0x34 a pointer to a list of 16-byte IVs, +0x38 their count (one per logical page the command moves,
  in entry order), +0x3c the data unit (0x1000, the 4 KiB logical page; 0 when the command wants no AES),
  +0x40 the cipher (3, AES-256), +0x44 the 32-byte key, the file's own key as HFS content protection
  unwrapped it. The FMI runs AES-256-CBC over each logical page with that page's IV: encrypting on the way
  to the chips, decrypting on the way back. Erase commands carry none.
- **Measured** with the command's fields traced and the stored pages read from the `.pages` files: pages
  of a preferences plist decrypted offline (key and IV from the read command) to `bplist00` ...
  `AppleLanguages`. Single-page reads of small files each come with their own key; an 8-page read of a
  database with one key and 16 IVs.
- **Two write paths.** The kernel programs some protected pages through the inline AES and others it has
  already encrypted with the AES engine (`IOAESAccelerator` on the CDMA filter) as plain programs; it
  reads both kinds back through the inline AES. A store that skipped the AES both ways (format
  `ppn-xor-ff-v1`) gave back the first kind right and the second still encrypted, as soon as the buffer
  cache no longer held them: after a reboot every protected file written in the session read as garbage.
  In the install boot of a protected volume every write took the second path (no inline-AES program at
  all); in a normal boot most go through the inline AES.
- **The model** (`fmi_inline_aes`, `fmi_aes_page` in `s5l8940_iop.c`): for stores of format
  `ppn-xor-ff-v2` it decrypts each read logical page that is neither erased nor a served marker, and
  encrypts each programmed one, with QEMU's AES-256-CBC; an IV past the command's list or another cipher
  or unit is a page error (logged once). v1 stores keep the pass-through: they hold pages that inline-AES
  programs stored in the clear, which would read as garbage through it. `nand_ppn.py` writes v2.
- **Files without a key.** xnu gives a file that has no `com.apple.system.cprotect` attribute on a
  protected volume a new class D key when it is first opened, and reads its data through that key from
  then on. The install used to write the data volume unprotected and set kHFSContentProtectionBit at the
  end, so its files (the system keybag, launchd's overrides, the `/private/var` skeleton) were plaintext
  with no attribute: under the AES the keybag read as garbage ("FATAL KEYBAG ERROR: kb_load", launchd
  rebooted to recovery). `it_install` now protects the volume before mounting it, formats the effaceable
  storage before the mount (a protected volume mounted before the keystore has its lockers refuses every
  file creation, EPERM; `_MKBKeyBagCreateSystem: Can't save the bag`), then creates the keybag and the
  rest, as a restore does. `ios_prep.py` refuses the install boot on a v1 store: such a unit is made
  again (`device delete`, `device create`).
- Checked on a new v2 unit over the GL bridge (`runs/n94/progress/2026-10-18_AES_*`): Setup walked, QMP
  `quit`, the next boot at the lock screen, the home screen with the wallpaper and the whole Calendar icon.

## Storage: the page store on the host (2026-10-19)

What the FTL costs the PC, measured with the model's own counters (`store-stats`, a QOM property of
`/machine/iop`, and one line on QEMU's stderr at exit: pages read, programmed, erased, served as markers,
through the AES, page errors, and the host time of each) on 16 GiB units whose stores sit on the host's
hard disk (E:, an HDD) with the firmware on its SSD. A cold random 8 KiB page read on that disk costs about
13 ms, a cached one 9 us.

| | before (2026-10-18) | after |
|---|---|---|
| `device create` (install boot: FTL format, 312,445 markers, data volume, keybag) | 97 s; the store 52 s of the 87 s boot | 69 s; the store 29 s of 64 s |
| new unit's first boot to Setup's welcome ("iPhone", slide to set up) | 175 s; the store 68 s, 33,125 host page reads | 115-120 s; the store 8 s, 1,831 host page reads |
| normal boot of `gl5` to the lock screen | still on the boot logo at 120 s | 70 s (110 s on a session with no `pagemap.bin`, below) |
| install boot of a 4 GiB unit whose FTL must collect garbage (`device refresh`) | 135-153 s, 1,240 erases in 81-85 s | 78-97 s, 1,284 erases in 14-35 s |

Where it went, and what changed in `hw/arm/s5l8940_iop.c`:

- **Programs read before writing.** Flash programming only clears bits, so every program read the page
  back first. The sparse chip files' holes are erased pages (stored XOR 0xff), so at open the store asks
  the host which ranges are allocated (`FSCTL_QUERY_ALLOCATED_RANGES` on Windows, `SEEK_DATA`/`SEEK_HOLE`
  elsewhere) and keeps a per-page bitmap of pages known erased; an erase sets its block's bits, a program
  clears its page's. A page known erased is read as 0xff with no I/O, programmed with one write and no
  read, and a block known erased is erased with no I/O at all (16,384 such erases in a format).
- **Erases read the block.** To leave a hole alone, each erase read the whole 1 MiB block and wrote
  zeros only if it found data; on a block the guest had written (garbage collection) that read was a seek
  and the erase cost 68 ms. With the holes known, an erase writes its zeros through the host's cache with
  no read (11-27 ms). Punching a hole instead (`FSCTL_SET_ZERO_DATA`) was tried and was slower: NTFS writes
  the range's dirty cache back synchronously before it deallocates.
- **One host I/O per run of pages.** A PPN read or program command moves up to 128 pages; the pages that
  follow each other in the chip file (the FTL fills a block page after page) now go in one read, or in one
  write when they are erased pages being programmed.
- **The marker pages.** About 21,000 of the 25,000 pages a first boot reads are system-volume marker pages
  (B3), and each read went to the chip file only to find 28 bytes of marker and the page's meta: on the
  HDD, a seek apiece. The store now keeps a map of the stored pages that are whole marker pages (every
  logical page an exact marker, the rest zeros): their meta and the {source, block} of each logical page,
  48 bytes a page, 8.7 MB for a unit. Reading such a page rebuilds it from the map and reads only its
  source (the IPSW's `rootfs.hfs`). Programs and erases update the map and bump the block's generation.
  Between sessions the map is `pagemap.bin` beside the store: written at QEMU's exit (also after the
  guest's own power-off) and deleted as soon as it is loaded, so a session that ends without its exit (a
  killed QEMU, a host crash) leaves none. Without one, a thread scans the store at low I/O priority,
  giving way to the guest's reads (1.4-1.7 GB in 32-40 s on the HDD), and adds the marker pages of every
  block whose generation did not change while it read. Pages the guest reads are added as they come. The
  install boot programs the markers through the store, so a new unit's first boot already has the map.
- **The host's cache does not hold the store.** A read-ahead thread that read every allocated range at
  start (to warm Windows' file cache for the guest) was tried first: the pages were gone again within
  minutes, and in a thread in background mode Windows also gives what it reads low memory priority, so
  the guest's reads stayed cold. Hence the map in the model's own memory.

**A long session** (`wear4`, a 4 GiB unit of 976,563 sectors; `device create` then 12
`device refresh`, each rewriting all 312,445 markers, 1.2 GB, into an FTL with 3.7 GB, so from the second on
the FTL collects garbage): about 157,000 page programs and 1,200-1,290 block erases (garbage collection)
in each session, 2 million programs in all. In every session: 0 page errors, 0 bad markers, the data
volume's `fsck_hfs -fy` "appears to be OK" and the keybag found. Wear per session: 1,200-1,290 of the 4,096
blocks erased, 1.01-1.02 erases each on average and 2 at most, so the FTL spreads its erases. (The model
counts erases per block for a session only; a lifetime count would need them in the store.)

## Power: off, restart, and a session cut short (2026-10-19)

Three ways a session ends, each checked on `gl5` (a unit past Setup) by the next boot reaching the lock
screen with no Setup and launchd's `fsck_hfs` on both volumes saying `QUICKCHECK ONLY; FILESYSTEM CLEAN`
(`runs/n94/progress/2026-10-19_PERSIST_desligar-reboot-kill.png`):

- **"Slide to power off".** Holding the power button 4 s brings the slider; sliding it makes launchd stop
  every job (about 21 s) and call `reboot(RB_HALT)`. The kernel then hung with the CPU idle: a gdbstub
  look at launchd's thread (its kernel stack's saved frame chain, mapped to kexts through the prelink info)
  showed it in IOMikeyBusFamily's shutdown under AppleCS42L63Audio, which powers the codec down (`0x06` bit
  0) and reads `0x62` every 10 ms until bit 7 says it is down, with no limit (10B329 `0x80acbebc`). The
  codec was the A4 boards' plain register file; it is now a `cs42l63` type of that model that sets the bit.
  Past it, the kernel flushes the FTL (`AppleNANDFTL::systemWillShutdown`) and NVRAM and halts in the PMU
  driver (`0x80aed7a0`): with no cable it sets `0x15` bit 0 ("pmu go stdby") and waits for the power to
  go; with a cable, which the model always has (the D1881's VBUS), it writes `0xaf` <- 0x90 and restarts,
  for iBoot to show the battery charging with iOS off. The D1881 model powers the machine off in both
  cases (QEMU's guest shutdown: the process exits by itself about 25 s after the slide); a restart with no
  halt before it (`0xaf` untouched) resets the machine as before. The next boot opens the FTL with no
  "unclean shutdown". `tools/dev/n94_input.py poweroff` does the whole gesture.
- **A restart asked by iOS.** Ajustes > Geral > Redefinir > Redefinir Ajustes de Rede (nothing to lose on a
  unit with no network) ends in launchd's `reboot(RB_AUTOBOOT)`: the FTL and NVRAM flushed, `pmu restart`
  (`0x96` <- 0x2b, no `0xaf`), the machine reset by the model, and the next boot is clean.
- **QEMU ended with no shutdown at all** (`quit` on QMP or the monitor, the window closed, the process
  killed): for the guest a power cut. The FTL recovers as it would from a pulled battery (`sftl: error,
  unclean shutdown; adopted N spans`), the data volume is journaled (`newfs_hfs` gives it an 8 MiB journal,
  mounted `journaled, protect`) and its quick check is clean, and the unit is at its lock screen.
  `pagemap.bin` is not there after a kill and is rebuilt in the background (above).

The install boot ends the same way now: `it_install`'s halt powers the machine off, and `device.py` waits
for that (up to 90 s) instead of quitting QEMU 3 s after `it_install: done`, which had left the FTL to
open the next boot with "unclean shutdown".

## Boot: what a normal boot logs, and what still falls (2026-10-19)

Times (16 GiB units, stores on the host's HDD, one vCPU): a new unit's first boot reaches Setup's welcome
screen in 115-120 s (`BSD root` at 10 s, launchd at 10 s, the system keybag at 20 s, a short data migration
with its progress bar), and `gl5`'s normal boot (a unit past Setup, GL firmware) its lock screen in 70 s. (A
unit put through many install boots boots slower on a hard disk: `gl5` after the seven of 2026-10-19 holds
937,000 marker pages and 9 GB in its chip files, its 3,400 non-marker page reads at boot cost 10 ms each and
the lock screen came at 110 s.)

**The kernel's console** in a normal boot, every line that reports a failure, and why:

| Line | Cause | Emulator? |
|---|---|---|
| `wlan ... resetWaitTillEnumeration(): Error (I/O timeout) waiting for bus to attach`, `Bus enumeration failure` | no BCM4330 behind the HSIC port | yes, by design (radios are stubs) |
| `hsic-baseband ... device has not connected after 1500ms` | no baseband | yes, by design |
| `AppleH4CamIn::power_on_hardware - ISP_Init failed: 0xE00002D6` (three times) | the camera ISP is not modeled | yes, unmodeled block |
| `AppleJPEGHal::reset_jpeg_block() : SW Reset failed (2)` | the JPEG block (0x38200000) is not modeled | yes, unmodeled block |
| `AppleM2ScalerCSCHal::setScalerPropertiesAndExtents() WARNING: Failed to find properties ... SOC 0x0` | the scalers are not modeled | yes, unmodeled block |
| `AppleLIS331DLH::handleStart Expected calibration data but didn't find any.. Failing driver`, `AppleAP3GDL::probe -- Failed to Probe` | no factory calibration for the accelerometer and gyro (it lives in syscfg on a real unit) | yes |
| `AppleTVOut::start ... Couldn't find video LDO function` | no TV-out on this board's DT | no (same on hardware) |
| `[NAND] s_cxt_boot:88 sftl: error, unclean shutdown; adopted N spans` | the session before ended with no shutdown (QMP `quit`, a closed window, a kill) | gone after a power-off ("Power" above) |

None of these stops a daemon. **User space** writes no console here, so its failures were read from the
unit's own crash reports: the store model's `dump-writes` property (debugging only, off by default:
`-global driver=s5l8940.iop,property=dump-writes,value=FILE`) appends every logical page the guest
programs, and every page it reads back through the inline AES, in the clear; at boot `OTACrashCopier` reads
the crash log index, so one boot shows them all. What `gl5` recorded over 18 boots:

- **`assistantd`, every boot**: `mediaserverd: RPCTimeout message received to terminate [0] with reason
  'AudioSessionSetProperty'` (bug_type 188, a stackshot of every process). Fixed, see below.
- **`MobileMail` and `MobilePhone`, once** (the same boot, opened right after Setup): `failed to launch in
  time` after 20 s of CPU: the launch watchdog against the emulator's speed, not a missing device.
- Processes sampled over the gdbstub every 15 s (`xnu_procs.py`) show the on-demand daemons coming and
  going (securityd, calaccessd, backupd, softwareupdated, mediaremoted restart on demand, as launchd runs
  them), and no other crash report.

**Why assistantd died, and the fix.** Not the audio hardware: in a boot nothing plays sound, and the AE2's
audio rings and the I2S DMA stay idle on a real unit too. The stackshot's frames, symbolized from the shared
cache's exports with the boot's slide (the M3 tools: `ustacks.py`, `cacheslide.py`, `machosym.py`, and the
binary-image UUIDs of the report), and the processes sampled every 4 s show the sequence:

1. mediaserverd starts at ~25 s and spends ~27 s in `FigPlayerCMSessionOneTimeInitialization`: CoreAudio's
   HAL plugins (`VirtualAudio.plugin`), property lists, run-loop sources, and 5 s in a synchronous
   `_CTServerConnectionCopyAudioVocoderInfo` to CommCenter, itself still starting at background priority.
   Plain CPU work on one TCG vCPU standing in for the A5's two cores.
2. assistantd, started at load, calls `AudioSessionSetProperty` (through AVFoundation) meanwhile.
   AudioToolbox sends it as a MIG request with `MACH_RCV_TIMEOUT`; a gdbstub hardware breakpoint on
   CoreMedia's `FigRPCTimeout_KillAndForceCrashReport` caught the caller there with `0x10004003`
   (`MACH_RCV_TIMED_OUT`) and the timeout, 5000 ms, on the stack.
3. On that timeout AudioToolbox writes the stackshot report and the caller kills itself; launchd starts
   assistantd again later on demand.

On the phone mediaserverd's initialization fits in 5 s; here it does not. The limit lives in AudioToolbox's
`__DATA` (a global the MIG stubs reach through a non-lazy pointer), so every process starts with the
shared cache's value: `sysvol.audio_session_timeout` raises it to 60 s on the system volume, one of the
per-firmware edits `prepare` and `device create` apply ("Preparing the firmware" in README). It finds
the global from AudioToolbox's own code (the one word holding 5000 that its pc-relative pointer loads
reach inside its `__DATA`), so no address is assumed. Tried first and ruled out by the same breakpoint:
CoreMedia's `gFigRPCTimeout` (9 s, other calls), AudioToolbox's other 9 s limit, and the
`com.apple.coremedia` `rpc_timeout` preference (it sets `gFigRPCTimeout` in seconds, on the data volume;
`gl5` keeps that test file, harmless). With the edit, on `gl5` and on a new unit, assistantd lives until
its own idle exit and no report is written (below). icount (`-icount shift=0,sleep=on`) also avoided the
kill, by running the guest's clock on executed instructions, but the guest's time-of-day then fell behind
the host's by the boot's busy time; not adopted.

## SMP: the second Cortex-A9 (2026-10-19)

The machine has `-smp 1` or `2` (`device boot` passes 2, and the bundle's device tree shows both `cpus`
nodes with no `cpus=1` boot-arg; `ios-prep --cpus 1` keeps the old single-core bring-up). What the kernel
does to start cpu1, as traced and decoded:

- **AppleARMCPU::start** (AppleARMPlatform, 0x803c2ff0) counts the `/cpus` children (lowered by `cpus=`),
  creates the IOCPUInterruptController for that many, registers each core with `ml_processor_register`
  and `processor_start`s the secondary; `PE_cpu_start` reaches the DT's `function-enable_core`, which is
  the PMGR's platform function `'Core'` with argument `1 << n` (cpu0 1, cpu1 2).
- **'Core' in the PMGR kext** (0x808fe828; register writes go through its accessor at 0x808ff248):
  start = `0x1214 <- mask`, `0x1220 <- mask`, then `0x1204 <- 0`; stop = `0x1210 <- mask`. At start the
  kext writes `0x1200 <- 0x7ffe` and `0x1204 <- 0x3fff8001` (0x808feeec). The model's PMGR (`cores`
  property) raises its `core-enable` line n on the start and lowers it on the stop; `n94.c` powers the
  CPU on with `arm_set_cpu_on` at **PA 0**, SVC mode, MMU off (the 6.x kernel copies its exception vectors
  to PA 0 "for a core reset to land on", and its reset handler takes the secondary from there), or off.
  cpu1 is created `start-powered-off`; a machine reset powers it off again.
- **Interrupt delivery.** Every registered vector targets both cores (`TARGET_CPU` = 3), but one core
  takes it: delivered to both, the display pipe's handler ran on both at once and cpu1 panicked
  (`Unexpected Display Pipe interrupt, pssible hperf block corruption: 00000100`, the status cpu0 had
  already cleared). The AIC model now lets the core whose IACK reads a vector first claim it; the other
  core stops seeing it until the claimer's dispatch loop ends (its IACK reads "none"). Which core the
  hardware picks is unknown; nothing in the kext depends on it.
- **IPIs and the per-CPU timer** need nothing new: IPI_SET bit n reaches core n as "other", bit 31 the
  sender as "self", and cpu1's timer is its own bank (`0x5090 <- 1` at start), its FIQ wired to cpu1.
- **Two host threads (MTTCG).** QEMU runs each vCPU on a thread of its own, the default for this guest
  on an x86 host, and the only way that boots: with both vCPUs on one thread (`-accel tcg,thread=single`)
  the kernel panicked early (`spin lock timeout`), a core spinning on a lock the other only releases at
  the next round-robin slice while the timebase runs in host time. Two things had to follow:
  - **The GL bridge.** An EGL context is current on one thread at a time; `gles-host` keeps it current
    on the thread that called, and CoreAnimation's calls now also come from cpu1's thread: ANGLE refused
    the context there and the panel stayed black after the boot logo. The calls run on the trapping
    vCPU's thread (a call may raise a guest exception on its caller, `ARM_CP_RAISES_EXC`; running it on
    cpu0's thread instead crashed QEMU in `cpu_exec_longjmp_cleanup`), and a call from the other vCPU
    first has the holder's thread release the contexts (`gles_host_release_thread`, after landing the
    frame in flight) through `run_on_cpu`, outside its execution loop while it waits (`cpu_exec_end`),
    since the holder may be waiting for exclusive work (the guest's broadcast TLB maintenance): without
    that QEMU hung. Under 500 such hand-overs in a boot to Setup.
  - **The timebase without the BQL.** Every MMIO access takes the big QEMU lock, and `mach_absolute_time`
    (0x80087b20) reads TIME_HI, TIME_LO, TIME_HI on every call: sampled over the gdbstub, both vCPUs sat
    on that loop a sixth of the time. TIME_LO/HI are now a region of their own with lockless I/O.
- **Measured** (`gl6`, a 16 GiB unit at Setup's welcome screen, GL firmware, store on the host's HDD,
  the same QEMU binary): Setup's screen at **88 s with one core, 59 s with two**; before the lockless
  timebase two cores took 90 s. Both vCPU threads are busy for the first minute and nearly idle (`wfi`)
  after. Still on the table for speed: the kernel's `TLBIMVAIS` loops (0x80087350), each a cross-vCPU
  flush in QEMU (exclusive work per page).
- **The lock screen** (`gl6` past Setup, `device boot`): reached 54-55 s after the command with two cores,
  78 s with one. Ten minutes there with two cores, the panel woken by Home every minute: 10 of 10 wakes,
  SpringBoard, backboardd and mediaserverd alive, no panic, `hw.ncpu 2` at the end. Not SMP's: with one
  core as with two, mediaserverd is sometimes killed by CoreMedia's 9 s RPC timeout ("fig rpc timeout --
  playerremote_CopyProperty [ CallIsActive ]", read from the unit's own report through `dump-writes`), more
  often once system sounds play (unlock, clicks); launchd does not bring it back and SpringBoard then hangs
  in `AudioServicesPlaySystemSound`, so the panel no longer wakes. The audio path (the AE2's audio rings)
  is the place to fix it; open.
- **Checked**: `tools/dev/xnu_sysctl.py` reads the guest's hw counts over the gdbstub (`hw.ncpu: 2`,
  `hw.activecpu: 2`, `hw.physicalcpu: 2`, `hw.logicalcpu_max: 2`); the kernel has no `machine_info`
  symbol, so the tool takes its address from `host_info`'s literal pool (0x802d01a8 in 10B329).

## Platform: PMGR, watchdog, DWI, PL310, GPIO, the clock table (2026-10-19)

Every access the kernel makes to these blocks during a boot (to Setup and two minutes past it, two cores)
is now modeled: `-d unimp` shows none of them (what is left there is the JPEG, scaler, ISP, usb-complex
and i2s-switch blocks). Decoded from the 10B329 kexts (Ghidra):

- **PMGR** (`AppleS5L8940XPerformanceController`, AppleS5L8940X kext 0x808fc8f0): beyond the A4's PLLs,
  clock configs and gates, the model's `a5` property adds `0x38` (the CPU's performance state: written
  from `cpu-clk-cfgs`, polled until bit 30 clears), `0x300` (the SoC's: a byte, polled until `[23:16]`
  clear), `0x2104` (power domains: the kext sets bit 16 + n in its mask and polls until the register shows
  none of them pending, so `[31:16]` read 0), and configuration words written at start (`0x3c`, `0x2108`
  <- 0x51, `0x3100`-`0x3108` from the DT). `0x1200`/`0x1204` are the core control ("SMP"; `0x1204` <- 2 or 4
  also comes from a per-domain on/off path, 0x808fe578).
- **Watchdog** (`AppleS5L8930XWatchDogTimer`, the A4 kext, matched by `wdt,s5l8930x`): the A4's four
  registers at PMGR + 0x3020 (TMR, RST, INTR, CTL). Arming is TMR <- 0, INTR <- RST / 2, RST <- 10 s of
  ticks (`_wdtResetCount=0xe4e1c00`), CTL <- 0xd; the interrupt (AIC 4) handler sets CTL bit 1 and kicks;
  CTL <- 0 disarms. The kernel restarts the machine through it: the platform function `'WDTr'` writes CTL
  |= 4, RST <- 0, TMR <- 0 and spins. The model counts while CTL bit 0 or 2 is set, raises the interrupt
  at INTR (bit 1, write 1 to clear) and resets the machine at RST. Checked: a `panic()` injected over the
  gdbstub (pc = `_panic`) on a bundle without `debug=` (the default boot-args' `debug=0x8` makes a panic
  wait for a debugger instead) ends in `'WDTr'` (`-trace s5l8930_pmgr_wdog*`: `wdt +0xc <- 0x4`, `wdt
  reset: count 0 limit 0`) and the machine boots again to `BSD root`. Before, the same write fell into the
  PMGR's unmodeled registers and a panicking guest hung.
- **DWI** (`AppleS5L8940XDWI`, "using DWI for BUCK changes"): three channels with their control word at
  `+0x10`, `+0x20`, `+0x40` (0x71 starts a transfer; bit 0 busy, polled with a deadline and a panic) and
  data at `+0x14`, `+0x24`/`+0x30`, `+0x44`; `+0x00`, `+0x04`, `+0x50` timing. A register file whose busy
  bits read clear; the buck voltages are not passed to the PMU model.
- **PL310**: QEMU's `l2x0` for the controller, and its second window (`pl310` reg[1], 0x3FD00000,
  `AppleS5L8940XPL310`'s "cif": `0x1020` <- 1, `0x1120` <- 0x80000100) as a register file.
- **GPIO**: `0xc48` is the NPL mode word, which `AppleS5L8930XGPIOIC::start` sets to 1 unless the node has
  `no-npl-mode` (the A5's has not); kept and read back.
- **arm-io's own registers** (0x3FB00000 and 0x3FC00000, `AppleS5L8940XIO`): bus timeout (`+0x04` <- the
  DT's `bus-timeout`, then 0x01000000) and PIO error reporting (`+0x18` <- 1, `+0x1c` <- 3); register
  files that never latch an error.
- **The clock table.** A node's `clock-ids` entry 0x100 + n is slot n of `/arm-io/clock-frequencies`, the
  64 words iBoot fills. `AppleS5L8940XIO::start` types each slot itself (a table of four names per slot at
  0x809014d0): `nclk` 30 and 50-54, `hclk` 31, 33, 36-44 and 63, `pclk` 45-49. In the DT, i2c uses 30,
  mipi-dsim 32, ae2 33, usb-complex 36, clcd/rgbout/tv-out 38, iop 43, pl310 44, flash 46-47, uart and spi1
  48 (spi1's function clock 51), pke 49, vxd 62-63; ids under 0x100 (0x4 for wdt and uart, 0x5 for the
  OTG PHY) are not table slots, and the watchdog checks that its clock equals the timebase (it does).
  `ios-prep` writes 24 MHz to slot 0 (the oscillator) and the `nclk` slots, the bus clock (200 MHz) to the
  `hclk` ones and the peripheral clock (100 MHz) elsewhere. The real 4S values come from iBoot's PLL setup
  and are not known here (no public ioreg dump was found); nothing the models do depends on them.

## USB: the host (2026-10-19)

`tools/dev/usbmux_bridge.py` is the host side of the fork's tcp_usb transport (`docs/tcp-usb-protocol.md`),
written for Windows, where there is no usbmuxd: QEMU's OTG model dials it (`-M iPhone-4S,usb-tcp-addr=
127.0.0.1:1235`, or `IT_USB_TCP`), and it serves usbmuxd's client protocol on 127.0.0.1:27015, where the
libimobiledevice tools (MSYS2's `mingw-w64-ucrt-x86_64-libimobiledevice` 1.3.0) look for it. `device boot`
starts it and wires the link by default (`--no-usb` for neither). What it took:

- **Enumeration** as the built-in host model does it: tcp_usb hello (v1, max transaction 32767), reset and
  enumdone until the guest answers GET_DESCRIPTOR (it NAKs for the first ~60 s of a boot), SET_ADDRESS,
  each configuration until the one with the mux interface 255/254/2 (configuration 4, "PTP + Apple Mobile
  Device", bulk 0x85 / 0x04, 512), SET_CONFIGURATION, Apple's vendor power request 0x40/0x40 (500 + 1600
  mA, a charging port), the serial string (the UDID). The device is 05ac:12a0.
- **The mux** (usbmuxd's device.c protocol): version request, the device answers 2.0, then the v2 SETUP
  packet (`0x07`), which starts the sequence numbers at 0 for the packets after it ("Error: detected
  missing packet" from `AppleUSBDeviceMux` when the version packet was counted). iOS 6 sends its v2
  headers with **magic 0**, not 0xfeedface, on data and control packets alike. A TCP per client: SYN,
  the device's SYN|ACK, ACK, then data with its window honored and every segment acknowledged.
- **TLS.** `idevicepair pair` works as is; the SSL session after it failed (`SSL error (-5)`): iOS 6's
  lockdownd has no RFC 5746 secure renegotiation (`UNSAFE_LEGACY_RENEGOTIATION_DISABLED` in OpenSSL 3) and
  signs with SHA-1. `tools/dev/openssl-ios6.cnf` (via `OPENSSL_CONF`) allows both for those tools; the
  session then runs TLS 1.2, AES256-SHA256. Pair records and the host's BUID live in `<unit>/lockdown`.
- **Checked** on `gl6` booted by `device boot`: `idevice_id -l` lists the UDID; `ideviceinfo` (paired,
  SSL) shows ProductType iPhone4,1, ProductVersion 6.1.3, BuildVersion 10B329, HardwareModel N94AP,
  ActivationState Activated; the battery domain shows the level and charging; AFC (`com.apple.afc` through
  lockdown's StartService, a small Python client) lists the media root and round-trips 200 KB with the
  same SHA-256.

## PMU and sensors (2026-10-19)

- **Motion sensors.** iBoot copies the unit's factory calibration from syscfg into properties the IPSW DT
  reserves zeroed: `accel-sensitivity-calibration` and `gyro-sensitivity-calibration` (10 words; the
  drivers read 9, a 3x3 matrix in 16.16) and `gyro-sens-matrix-inverse`. `AppleLIS331DLH::handleStart`
  (AppleEmbeddedAccelerometer) fails its start when the matrix is all zero ("Expected calibration data
  but didn't find any.. Failing driver") unless the node has `NoFactoryCalibration`; `AppleAP3GDL`
  (AppleEmbeddedGyro) inverts the matrix itself when the inverse's first word is 0 and stops on a singular
  one. `ios-prep` writes the identity to all three (kboot has no syscfg; `ipad1.c` does the same for the
  A4 boards). Both now start (`_calibrationMatrix [65536 0 0] [0 65536 0] [0 0 65536]`,
  `AppleAP3GDL::handleStart ... _odr200HzSampleInterval = 5000`). `AppleAP3GDL::probe -- Failed to Probe
  ... ID=0x0` stays for `gyro1` at 0x68: the DT lists both addresses the part can strap to, and a unit
  populates one.
- **D1881 ADC** (`s5l8940_d1881.c`, e2d2116bf1): the muxes AppleD1881PMU converts are identified (vbat and
  vcc_main 0/4, the dock's D+/D- 6, NTCs, tjunc 8, vbus 15). A boot converts mux 4 once (a4) and mux 3
  every ~2 s; muxes 0x0a-0x0e, 0x12-0x19, 0x22, 0x28 and 0x32-0x39 (with flag bits 0x80/0x40) about every
  7 s. `vbat-mv` is a property now (default 4037 mV), set from the machine's `battery-level`.
- **Gas gauge.** uart5 carries the bq27540 (`s5l8930_hdq.c`, the model the n90 board uses, with this
  board's 1432 mAh and DEVICE_TYPE 0x0540). UserEventAgent's AppleHDQGasGauge plugin (shared cache
  0x38144000) writes Control(FW_VERSION) at start and reads 0x0109 back ("firmware 1.09"), then made no
  other HDQ transaction in six minutes; it polls on something the kernel side was not seen to send
  (`AppleHDQGasGaugeControl` exposes the gauge's SWI line, PMU GPIO 5, and a data-log buffer).
- **The battery level.** Lockdown's battery domain (through the USB bridge) settled it: the level iOS
  reports (`BatteryCurrentCapacity`, what the status bar draws) is AppleD1881PMUPowerSource's look-up, at
  boot, of the vbat it reads in its `battery-data` curve, the `0003-default` entry of its IOKit
  personality (family 3, the charger node's `battery-family`; percent and 16.16 mV pairs: 100 % 4200 mV,
  82 % 4047, 50 % 3877, 9 % 3742, 2 % 3450, 0 % 3000). The machine's `battery-level` sets vbat from that
  curve: 50 reads 53 %, the default 80 reads 84 %, charging. A change at run time does not reach the
  guest (nothing re-reads vbat; the gauge is only asked its firmware version). Setup's status bar draws
  the charging battery full whatever the level (the lock screen and home screen draw it; not seen here,
  the test units being at Setup).
- **Buttons and wake events.** The DT's `wake_button_menu`/`wake_button_hold` are the PMU function
  `'STAT'` 0x180/0x181. AppleD1881PMU's handler for it (0x80aee18c) reads, for an argument with bit 7, the
  event registers latched at the last PMU interrupt (register `(arg & 0x3f) >> 3`, bit `arg & 7`), else
  STATUS_A + that register, inverted by the function's second byte: Home and power are EVENT_A bits 0 and
  1 (dock `acc_detect`, `'STAT'` 5, is STATUS_A bit 5). The PMU model latches them when the machine's Home
  and power keys go down (gpio-in `button`, wired beside the GPIO pins). Awake the kernel keeps them
  masked (IRQ_MASK_A ends at 0xc7) and reads them with the other events: `-trace s5l8940_d1881_read` shows
  EVENT_A 0x01 after a Home press and 0x02 after power. They tell a wake's reason; the AP's sleep itself is
  not exercised on a USB host's cable (iOS keeps the AP awake there).
- **Checked on the lock screen** (`gl6` walked through Setup, two cores): the status bar shows 84 % and the
  charging glyph, the lock screen the charging battery; Ajustes > Geral > Uso > Nível da Bateria shows the
  percentage.

## Display: the pipe as iBoot leaves it, and the A5's layer MMU (M3 work, 2026-10-17)

The A4 display model (`hw/arm/s5l8930_display.c`: DisplayPipe0, CLCD, RGBOUT and its control block, TV-out)
is instantiated at the A5's addresses with `generation=4`, and the Samsung MIPI-DSIM model
(`hw/arm/ipod_touch_mipi_dsi.c`) at 0x39500000 (IRQ 41, 4 lanes, the HS clock request already on, as iBoot's
`pinot_init` leaves it). The two DARTs `AppleS5L8930XDART` programs (dart-nrt, dart-rt) are mapped; the `clcd`
node has no `iommu-parent`, so the pipe's layer bases are physical addresses (`AppleCLCD, fMapper: 0`) and the
model's own dart2 window stays unmapped. What the 10B329 drivers expect, and what the model does:

- **The gate.** `AppleCLCD::start_hardware` (the 6.x build, `AppleH4PCLCD`) adopts the display only as iBoot
  hands it over: the CLCD's timing generator running (CLCD `+0x50` bit 0, ENVID) with a nonzero vertical
  timing word (`+0x58`), and UI0 live on the pipe (`+0x1038` bit 8, the framebuffer base, stride and window).
  Otherwise it logs `CLCD not initied by iBoot, driver will not load` and never registers a framebuffer, so
  IOMobileFramebuffer has no main display for backboardd. The model's reset leaves exactly that (the same
  values the A4 boards get, the k48 timing words included) with the framebuffer from the kboot bundle's
  boot_args (`Video`: 0x9f700000, 640x960, 2560 bytes per row, which tools/ios-prep reserves under pram).
  The kernel then logs `DefaultFrameBuffer DPCENAB= 0x100, DPU0SRCFMT=0x0, DPU0SRCBASE=0x9f700000`,
  `fDefaultFBStride=2560, fDisplayWidth=640, fDisplayHeight=960`, `ApplePinotLCD: _lcdPanelID: 0x00a1d13c
  _lcdPanelClass: 2` (the DT's `lcd-panel-id`; nothing is read over the DSI link) and `Added framebuffer
  device: AppleCLCD`; `AppleARMBacklight` finds the CLCD service.
- **The A5 pipe's window registers.** `AppleH4PDisplayPipe` keeps the A4's `+0x40` format, `+0x44` base
  and `+0x48` stride in each UI block (`+0x4000` UI0, `+0x5000` UI1) but moves the window: source crop
  `+0x4c`, source size `+0x50`, destination origin `+0x60`, destination far corner `+0x70` (start_hardware
  takes iBoot's width as `0x70.hi - 0x60.hi`). The model reads those for generation 4 and seeds them at reset.
- **The layer MMU.** In place of the A4's DART the A5 pipe has an MMU per layer: `+0x90` bit 0 enables it and
  `+0x98 + 4k` holds the physical address of the page table of the layer's k-th 4 MiB (1024 words, `PA | 1`
  per 4 KiB page, filled by the driver's `populateUIMMUTable`); with it on, the layer's base is an address in
  that space. The model walks the tables when it fetches the layer. What backboardd's first swaps program
  (`-trace 's5l8930_display*'`, `[disp] pipe0 swap N: pkt`): UI0 format 0x1001 (BGRA), base 0, stride
  0xa02, source and destination 640x960, MMU on, and a page table that maps the surface 1:1 onto iBoot's
  framebuffer (PTEs 0x9f700001, 0x9f701001, ...): backboardd adopts the boot framebuffer as its first
  frame, as it does on the devices (the boot logo stays until the system app's first frame). The blend
  words at `+0x2040/+0x2044/+0x2048` (0x00ff0202, 0x00ff0001, 0x00ff0003) and the video block's bases
  (`+0x3074..+0x30a0`, all 0) ride the same swaps. Formats: 0 BGRA, 2 ARGB4444, 3 ARGB1555 (`L555`),
  4 RGB565.
- **Base 0 is a live layer.** Behind the MMU the base register is 0 on every swap; the A4 scanout took a zero
  base for an unset layer and kept the panel dark. The model now shows any MMU-on layer whatever its base.
- **What backboardd draws.** Three swaps on pipe0 in the first 90 s of a normal boot (IDs 9, 17, 25; the
  RGBOUT pipe gets a pair per `AppleRGBOUT` power cycle). 9 and 17 show UI0 alone, iBoot's framebuffer
  (black when this was traced; the bundle now carries the IPSW's boot logo, README "Preparing the firmware"). 25 turns on both layers (`0x1038` = 0x300), UI0 and UI1 each through its
  own MMU (page tables at 0x81cbc000 and 0x81d1e000 in one run), both BGRA 640x960 at stride 2560, blended
  UI1 over UI0: backboardd's software CoreAnimation (`CA_ENABLE_OGL=0`) shows the Apple logo with an empty
  progress bar, its boot overlay, and keeps it until the system app's first frame
  (`runs/n94/progress/2026-10-17_M3_primeiro-frame-logo.png`).

## The AE2 (`AppleA5AE2-43.1`, EmbeddedIOP role AE2, M3 work 2026-10-18)

Decoded from the 10B329 kexts (Ghidra; `AppleA5AE2::start` 0x8040148c, `startCPU` 0x80401b5c, its interrupt
handler 0x804018b0) and the firmware image (`IOP_s5l8940xae2_firmware`, 420 KiB, `cnfg` at 0x50080, an
iBoot-1537 task kernel with "iop", "console", "iop message" and "audio" tasks and the AAC codecs).

- **Windows** (the ae2 node's reg, in the kext's order): 0x341E0000 the core's control block, 0x341E1000
  its mailbox, 0x341A0000 a power/clock block, 0x341C1000 a configuration block, 0x34000000 192 KiB of
  SRAM (`sram-available`; the kext saves and restores it around sleep). Interrupt 0 is AIC 97.
- **startCPU**: control `+4` <- 0; power `+0` <- the `clock-dividers` word, then up to three polls of
  `+0` bit 16 (busy); control `+8` <- the firmware's physical base (the image runs in place in the firmware
  kext, 0x80b5d000); configuration `+0x14` <- 0x03ff01ff and `+0x34` <- 0x01000102 (ae2-version 1); control
  `+4` <- 0x003f0003 (run; 0x00ff0003 for later versions, bit 8 for a wake source); mailbox `+8` <- 0x10,
  then 0x20; the interrupt enabled. Power `+0x10..+0x20` <- 1 and back come first, from start.
- **Doorbell and answer**: the AP rings with mailbox `+0x20` <- 0x10 (0x20 for its second source); the
  interrupt handler writes mailbox `+0x24` <- 0x10 (acknowledge) and lets EmbeddedIOP walk the rings.
- **Rings**: EmbeddedIOP's, as on the IOP (64-byte entries, item | ownership bit), at the AP's physical
  addresses plus 0x40000000. The kext registers rings 0 (control, 2 entries), 2, 3, 4, 7 (2 each) and 5, 6
  (3 each). Only control messages have come so far: 'nop ' at start and as a keepalive.
- **Model** (`hw/arm/s5l8940_ae2.c`): those registers, the SRAM as RAM, the control endpoint answered as
  the IOP's, every other endpoint's message traced (`-trace 's5l8940_ae2*'`, `ep%d item ...: 8 words`) and
  handed back. That is enough for AppleAE2Audio's MCA (the I2S switch at 0x3FA01000, routes computed by the
  AP and written there; still an unimplemented window), AppleCS42L63Audio and Highland Park to start.
- **I2S**: AppleSamsungI2S (i2s-version 3) stops a port by writing `+0x00` <- 0x34 and spinning with
  `IODelay(10)` until bit 1 (drained) is set: on the unimplemented window that pinned the CPU from the
  codec's start on. The four ports are now the A4's `s5l8930_i2s.c` at 0x34190000 + n * 0x1000.

## Open points for the machine model

1. ~~AIC register map~~: confirmed against the kext (section above). Still open in the model: which GLB_CFG
   bit gates the timer FIQ, whether the timer reload is a true auto-reload or a count that keeps running,
   which core the hardware hands a vector targeting both (the model: the first IACK claims it, "SMP"),
   and what the kernel expects of an IPI it sends itself (0x2008 <- 0x80000000 while the IPI vector is
   still masked; with the clock running it moves on). cpu1's timer FIQ is wired since the SMP work.
2. ~~**PMGR timer / clock table**~~: done 2026-10-19 ("Platform"). Open: the 4S's real clock values (the
   slots' kinds are the kext's; the frequencies are plausible stand-ins).
3. **Unmodeled blocks the kernel writes at boot** (reads come back 0, nothing complains): CDMA 0x37000008
   and 0x3700000c (one bit per channel, all 44), usb-complex 0x3F108000, i2s-switch 0x3FA01000-0x3FA0103C
   (all 0). (The PL310 "cif" window, the DWI and arm-io's registers are modeled since 2026-10-19.) `AppleH4CamIn::start`
   (isp) and `AppleSamsungDPTXControllerS5L8940X` start without faulting on the unimplemented window.
4. **PMU**: `hw/arm/s5l8940_d1881.c` models what AppleD1881PMU (text 0x80aea268, decompiled with the
   Ghidra project) is seen to use: EVENT_A..G at 0x01-0x07 (read 7 at once by the interrupt handler,
   clear on read; EVENT_B bit 0x20 = ADC done), IRQ_MASK_A..G at 0x0e-0x14 (1 = masked), the ADC (0x43
   control with 0x20 = start, 12-bit result as 0x44[3:0] | 0x45 << 4, mid-scale for every channel until
   they are identified), the RTC counter at 0x5d-0x60 (32-bit seconds, the host's clock; the RTC class adds
   the offset it keeps at 0xa4) and the restart (0x96 <- 0x2b after 0xa1 <- 0, from the kernel's `pmu
   restart`, resets the machine). Its interrupt is GPIO 0x0d, active low. The driver's `ADC read bad state`
   and the 1970 date are gone; which ADC channel is what (battery, NTCs, dock) is still open, as are the
   wake events and buttons. Standby (`0x15` bit 0) and the halt-on-cable restart (`0xaf`) power the machine
   off ("Power: off, restart, and a session cut short").
   **USB**: wired (table above); restored's `enable_usb_connections` still fails and it waits for the
   `IONVRAM` resource, which on this NOR-less board comes from the NAND's partitions.
5. ~~I2C~~: decoded and modeled (section above).
6. **Storage**: decided by the user on 2026-10-17 ("Storage: the decision", option B) and done through B3
   the same day: PPN through a mailbox-level IOP model and a PPN page store, the guest formatting the NAND
   and installing the system volume on its first boot, and B4 (effaceable storage and the system keybag,
   keybagd running) the same day. The data volume is journaled (an 8 MiB journal), and the PMU's standby and
   the codec's power-down are modeled since 2026-10-19, so a halt flushes the FTL and powers off.
7. **Wi-Fi over HSIC**: an HSIC BCM4330 is a new device; a stub (device absent) suffices until M4.
8. ~~**SMP**~~: done 2026-10-19 (section "SMP"): enable_core is the PMGR's `'Core'`, two vCPUs on MTTCG.
9. ~~**Audio, the M3 gate**~~: the AE2 is modeled (section "The AE2" above), the codec, Highland Park and
   mediaserverd start. Open: no PCM reaches the host (no I2S DMA was seen; i2s0 has no "audio-out" yet), and
   the AE2's audio endpoints (rings 2-7) have not carried a message yet, so their protocol is unread.
   (assistantd's kill at boot, first put down to this, was a 5 s audio-session timeout against the
   emulator's pace: "Boot: what a normal boot logs".)
10. **Marker pages and the FTL's GC**: a marker page the FTL moves becomes a copy of the bytes it was
   served, so system-volume edits stop reaching the guest for the pages moved; the install boot rewrites
   all markers. A page store that kept the logical page's marker through a GC copy would make this go away.
