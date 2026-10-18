# OpenGL ES on the host GPU, on Windows (ANGLE over Direct3D 11)

The fork's GL bridge, as on the A4 boards (docs/n90/README.md, docs/ipad1/guest-services.md), on a Windows host:
the guest's OpenGL ES calls are trapped to QEMU and executed by `hw/arm/gles-host.c` on the PC's GPU, through
ANGLE (EGL + GLES 1.1/2.0 over Direct3D 11). On macOS nothing changes: CGL, or the experimental Metal ANGLE build
(`ltm_angle_prefix`).

```
guest app / backboardd ── QuartzCore ── OpenGLES.framework (contrib/gles-public, the front end)
                                              │ mcr p15,3,Rn,c15,c15,0   (QEMU_CALL, QC_GLES)
QEMU: hw/arm/n94.c n94_qemu_call ── guest-gles.c ── gles-host.c ── gles-host-angle.c.inc
                                                                       │ EGL / GLES 1.1 or 2.0
                                                          ANGLE (libEGL, libGLESv2) ── D3D11 ── GPU
```

## Building QEMU with the bridge

Dependency: MSYS2's `mingw-w64-ucrt-x86_64-angleproject` (in the workspace's portable MSYS2,
`toolchain/msys64`). `meson.build` looks it up with pkg-config (`angleproject.pc`) when the host is Windows;
found, `gles-host.c` is built with `GLES_HOST_ANGLE` and `GLES_HOST_ANGLE_D3D11`, not found, `gles-host-stub.c`
(every GL call refused, the guest stays in software) as before. A tree configured before the package was
installed needs `meson setup --reconfigure` (or `toolchain/build-qemu.sh configure`).

```
MSYSTEM=UCRT64 MSYS2_PATH_TYPE=minimal CHERE_INVOKING=1 ..\toolchain\msys64\usr\bin\bash.exe -lc "toolchain/build-qemu.sh build"
```

At run time QEMU needs `libEGL.dll` and `libGLESv2.dll` from `toolchain/msys64/ucrt64/bin` on the PATH (the
`device` commands and `run-*.sh` already put that folder first).

What the ANGLE backend does differently from CGL (`gles-host-angle.c.inc`):

- One native context per guest context, of the guest's API: ES 1.1 for an ES1 EAGL context, ES 2.0 (ANGLE hands out
  3.0) for an ES2 one. A guest sharegroup shares through a root context; ANGLE shares between ES1 and ES2.
- Each context draws into its own FBOs; the EGL surface is a 1x1 pbuffer, never swapped. Frames reach the guest as
  before: read back into the guest's IOSurface memory, which the display pipe model scans out.
- Framebuffer objects by the context's spelling: ANGLE takes `glBindFramebufferOES` and friends only in an ES1
  context and `glBindFramebuffer` only in an ES2 one (`GL_INVALID_OPERATION` otherwise). Measured with a probe
  program on ANGLE 2.1.25748 / D3D11 (Radeon RX 5500 XT): OES path complete in ES1, core path in ES2, BGRA
  readback (`GL_EXT_read_format_bgra`) and `GL_DEPTH24_STENCIL8_OES` in both.
- GLSL ES 1.00 goes to ANGLE as the guest wrote it (CGL needs it rewritten as desktop GLSL 1.20).
- No snapshots (`gles-host-snapshot.c.inc` is CGL-only): a migration blocker is held while contexts are live.

## The guest side: the front end, built without macOS

`contrib/gles-public/build.sh` needs ld64, lipo and an iOS 3.x SDK. On Windows `tools/ios-prep/gles_frontend.py`
builds the armv7 slice iOS 6 runs, with the workspace's clang and no SDK:

```
..\.venv\Scripts\python.exe -I tools\ios-prep\gles_frontend.py build OUT_DIR
..\.venv\Scripts\python.exe -I tools\ios-prep\gles_frontend.py install path\to\rootfs.hfs OUT_DIR\OpenGLES
..\.venv\Scripts\python.exe -I tools\ios-prep\gles_frontend.py software path\to\rootfs.hfs    # back to CA in software
```

- `contrib/gles-public/darwin-include/pthread.h` is the only header `opengles.c` takes from an SDK (Darwin's 32-bit
  ARM pthread types; everything else it declares itself).
- `tools/ios-prep/macho_dylib.py` links the one object into an `MH_DYLIB` dyld-212 loads: LLVM's `ld64.lld` has no
  armv7. Imports are bound with the flat-lookup ordinal from the four libraries build.sh links (libSystem, libobjc,
  CoreFoundation, Foundation); the export trie is exactly `opengles.exports`. `--dump FILE` lists sections,
  relocation kinds and imports.
- `install` writes `/System/Library/Frameworks/OpenGLES.framework/OpenGLES` (a new file: on 5.x+ the stock one is
  only in the shared cache), dyld's `/System/Library/Caches/com.apple.dyld/enable-dylibs-to-override-cache`, and
  takes `CA_ENABLE_OGL=0` (`sysvol.UNIT_EDITS`) off backboardd and SpringBoard. New files are made with
  `hfs.Volume.create_file` (a catalog record into a leaf with room; no node split).

## Measured (iPhone 4S, iOS 6.1.3, 2026-10-18)

iOS 6.1.3 on `-M iPhone-4S` boots to Setup with backboardd's CoreAnimation compositing through the bridge
(`runs/n94/progress/2026-10-18_GL_setup-boas-vindas-angle.png`; host log: `[gles-angle] context api=2 ...
OpenGL ES 3.0.0 (ANGLE ...) Direct3D11`, `CoreAnimation composites through the host`). Against software
CoreAnimation on the same unit (two boots each, numbers in docs/n94/README.md "GPU"): boot to Setup's
welcome page 193/263 s in software, 208/223 s through the bridge; its animation 8.6/9.3 against 10.2/9.1
frames/s. The same within the spread; the profile below says why, and it is neither the bridge nor the TCG core.

## Where a frame's time goes (2026-10-18)

Measured on the same unit with `-trace gles_obj_log` (every 10 s: calls, draws, flushes, the host time in
them, the traps that carried them and the frames written back), `-trace gles_call_log` (every call at
guest-virtual microseconds, switched on for a second with QMP `trace-event-set-state`, in order with the
display's `s5l8930_display_log` swap lines), the display's `swaps` and `frame-timeline` properties (one entry
per VBL), the host's per-thread CPU time and gdbstub PC samples. Setup's welcome page after a Home press:

| per CoreAnimation frame | |
|---|---|
| GL calls / traps | 60-63 / ~45 (the command buffer merges only 2 calls per batch trap: CA's frame is mostly calls that return something) |
| draws, flushes | 5, 1 |
| host time | 3.4 ms, of which 3.0 ms is the writeback (`glReadPixels` of the 640x960 BGRA surface, 2.4 MB, into guest RAM) |
| first to last GL call | 4.3-6.5 ms of guest time |

So the bridge costs ~6% of the wall time while the page animates. The guest is not busy either: the vCPU
thread runs at ~50% of a core (host GL included), and 69% of 659 PC samples were in the kernel's idle `wfi`
(0x8008714c on 10B329), 8% in a timer read loop, ~10% in user code. CoreAnimation swaps on every VBL but has
new content on every second one (the others re-swap the same buffer after 2 calls): this shimmer runs at 30
frames/s by itself. And `measure.py`'s frames/s is not a frame rate: the page animates for ~8 s after the
Home press, inside a 20 s window of ~50 screendumps/s.

What did limit it: **the VBL**. The A5 display model's 60 Hz tick (`hw/arm/s5l8930_display.c`) was re-armed
at "now + 16.7 ms", so each tick's wake-up latency was added to the period: 17.4 ms (57 Hz) at the 1 ms host
timer, and 31.2 ms (32 Hz) for the 127 s an SDL window of QEMU sat behind other windows. Windows 11 stops
honoring `timeBeginPeriod` for a process whose windows are all minimized or covered: a probe's `Sleep(1)` went
from 1.8 ms to 15.7 ms a few seconds after its window was minimized, and back to 1.8 ms with
`PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION`. Every guest timer fires that late too. Two fixes:

- `os-win32.c` opts QEMU out of that throttling (`SetProcessInformation(ProcessPowerThrottling)`, next to the
  `timeBeginPeriod` it already made);
- the VBL runs on a fixed grid (a late tick is followed by an early one; more than a period behind, the grid
  restarts from now, as `ipod_touch_lcd.c` does).

A/B on the same unit, the SDL window minimized right after start, the same three traced seconds after a Home
press:

| build | VBL while animating | new frames/s | `measure.py` (screendumps) | boot to the welcome page |
|---|---|---|---|---|
| before | ~33 Hz (43.2 Hz over 40 s) | 20.1, 20.7, 20.1 | 9.24 | 213 s |
| after | ~54 Hz (58.4 Hz over 40 s) | 30.4, 29.3, 30.7 | 8.60 | 243 s |

The page now reaches its own 30 frames/s with the window minimized (26.5 before with it in front, at 57 Hz).
The screendump number does not move, as above, and one boot each is inside the 193-263 s spread. What is left
on the host is the writeback; it would matter to a guest that renders every VBL, which this page does not.

## A page transition, frame by frame (2026-10-18)

A screendump sampler had caught a Setup page slide ("Voltar") in one intermediate frame, against the ~20 a
phone draws in its 0.35 s. Counted instead by the display's `frame-timeline` and `-trace gles_call_log`
(which now gives each call's host microseconds), switched on with `trace-event-set-state` around a tap, on a
clone of the M4 unit (`m4gl`) with the SDL window in front:

| slide | VBLs from its first to its last drawn frame | frames drawn | VBLs without one |
|---|---|---|---|
| Setup, language -> country (push), 3 runs | 22-24 | 16-17 | 5-7 |
| Setup, country -> language (Back), 3 runs | 20 | 16 | 4 |
| Settings -> Safari (push), 2 runs | 23 | 19-20 | 3-4 |
| the same, SDL window minimized, 2 runs | 23 | 19-20 | 3-4 |

So the bridge draws the slide: 70-87% of its VBLs carry a new frame, minimized as well (the VBL fix above).
The VBLs are 15.4-18.6 ms apart throughout. A slide frame is 230-280 GL calls with 60-80 draws, and spans
~15 ms (10-17) of guest time from its first call to its last; its host time is 3.8 ms: 2.2 ms the writeback
(the `glBindFramebuffer(0)` that ends CA's frame), ~1.4 ms the draws (18-20 us each), 0.1 ms the flush.
The other ~11 ms is the guest's own. The frames missed are the slide's first, which spans 3-4 VBLs (50-67 ms;
host ~15 ms of it, 17 IOSurface binds at 0.34 ms each among them, the rest CA in the guest preparing the new
page's layers), and now and then a frame whose span crosses a VBL. Before the slide the app lays the new page
out and draws it: 330-480 ms from the tap to the first slide frame.

The guest is busy throughout: gdbstub PC samples (~170/s, across a push and a Back) show 85-90% in the idle
`wfi` before and after, and none from the tap to the slide's end: first the app's process, then the one
making the GL calls, with 40-60% of the samples in the kernel, mostly the interrupt path and the 64-bit
timebase read (`0x80087b20`, three MMIO reads; samples land on MMIO exits more often than elsewhere, so that
share is probably high). The interrupt load is not a storm: with `-trace s5l8940_aic*` the same over 1.5-2 s
idle and during a push, 140-155 timer FIQs/s, ~500 timer reloads/s, ~2,700 timebase reads/s.

The one intermediate frame was the sampler. Screendumps as fast as QMP answers (76-100/s, `screendump` on
the main loop) during the same taps: the VBL went from 60 Hz to ~30 Hz with gaps up to 195 ms, taps were
lost, and the slides came out with 0-7 frames (0-1,236 GL calls in the 3 s traced, against 4,000-9,000
undisturbed). Count frames with the `frame-timeline`, never with screendumps. What would add frames is the
writeback's 2.2 ms, read back asynchronously (a pixel-pack buffer and a fence: the context is ES 3.0 on
ANGLE), which would keep the spans under a VBL; the first frame and the ~11 ms a frame takes the guest are the
TCG core's speed.

## Limitations

- Windows only with Direct3D 11 (ANGLE's default here); the Vulkan and SwiftShader backends in the package are not
  selected.
- No GL snapshots (savevm/migration) while a guest GL context is live.
- `gles_frontend install` edits a system volume image every unit of that firmware shares; a unit that should stay
  in software needs its own image or `gles_frontend software`.
