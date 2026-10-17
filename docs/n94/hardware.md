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
| Timers | inside PMGR (`device_type timer`) | no separate node; the A4 kernel's event timer is PMGR+0x2000 (`s5l8930_pmgr.c`). To confirm on A5. |
| Watchdog | 0x3F103020+0x10, AIC 4 | `wdt,s5l8940x|s5l8930x|s5l8920x` **A4** (inside the PMGR window). |

`arm-io` ranges: child 0x0 -> parent 0x30000000, size 0x10000000. Every `reg` below is absolute.
The A4's peripheral windows at 0x8xxxxxxx and 0xBFxxxxxx reappear at 0x3xxxxxxx and 0x3Fxxxxxx
with the same low bits: the display, DMA, USB, UART, SPI and PKE blocks keep their A4 offsets.

## Interrupts: the Apple AIC

| Item | A5 (n94) | Notes |
|---|---|---|
| AIC | 0x3F200000+0x10000, `aic,1`, `#main-cpus` | **new**. Replaces the A4's four PL192 VICs (0xBF200000). Driver `AppleInterruptController`. The Linux `apple-aic` driver (M1, AIC v1 register layout: event register, hw IRQ mask/unmask, IPI) and n94linux are the references; the A5's register map has to be confirmed against the kext. |
| GPIO | 0x3FA00000, AIC 119, `gpio,s5l8940x|gpio,s5l8930x`, 2 interrupt cells | **A4** `s5l8930_gpio.c` (the A4 GPIO is VIC 0x74). Buttons, PMU, touch, sensors and the radios interrupt through it. |

## Platform blocks

| Node | reg | AIC | compatible | Model |
|---|---|---|---|---|
| pmgr | 0x3F100000+0x7000; 0x38C00000, 0x38D00000, 0x38E00000, 0x38F00000, 0x3A500000, 0x3A600000 (+0x1000 each) | | `pmgr,s5l8940x` | **A4** `s5l8930_pmgr.c` (clocks, gates, timer); the A5 adds voltage/performance tables and six extra windows. |
| dwi | 0x3F700000, AIC 6 | 6 | `dwi,s5l8940x` | **new** (`AppleS5L8940XDWI`): single-wire PMU buck control; the A4's SWI (0xBF600000) plays the same role. |
| iop | 0x3F300000 | 2 | `iop,s5l8940x` | **A4** `s5l8930_iop.c` + `s5l8930_iop_core.c` run the IOP firmware (`IOP_s5l8940xiop_firmware` in the kernelcache). A4 IOP is 0x86300000 + VICs at 0xBF300000. |
| ae2 | 0x341E0000, 0x341E1000, 0x341A0000, 0x341C1000, 0x34000000+0x30000 | 97, 98 | `ae2,s5l8940x` | **new / hide**: audio engine (`AppleA5AE2`, `IOP_s5l8940xae2_firmware`). |
| cdma | 0x37000000+0x2D000, 0x37800000+0x9000 (AES) | 51..94 | | **A4** `s5l8930_cdma.c` (0x87000000 / 0x87800000, VIC 0x30+ch): same layout, 44 channels. |
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
| i2c0 | 0x33200000 | 18 | `i2c,s5l8940x|iic,soft` | **new driver** (`AppleS5L8940XI2C`); A4 is `i2c,s5l8930x` at 0x83200000 (`s5l8930_i2c.c`). Register layout to compare. Devices: PMU `pmu,d1881` @0x74 (GPIO irq 0xd), codec `audio-control,cs42l63` @0x4a, `headset-switch,ts3a8235` @0x3b. |
| i2c1 | 0x33300000 | 19 | same | `compass,akm8975b` @0x0d, `als,tsl2581` @0x49. |
| i2c2 | 0x33400000 | 20 | same | `accelerometer,lis331dlh` @0x19, `gyro,ap3gdl` @0x6a and @0x68. |
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

1. **AIC register map** (version 1 on the A5) and how the kernel routes the timer (FIQ on the A4).
2. **PMGR timer / clock table**: the A5's `clock-frequencies` index map; `ios-prep` writes the A4 map.
3. **I2C**: `i2c,s5l8940x` vs `i2c,s5l8930x` register layout.
4. **Storage**: raw NAND through the IOP's FMI firmware vs a PPN model.
5. **Wi-Fi over HSIC**: an HSIC BCM4330 is a new device; a stub (device absent) suffices until M4.
6. **SMP**: `function-enable_core` / `ipi_dispatch` on `cpus`: what the kernel pokes to start cpu1.
