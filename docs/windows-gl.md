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
ANGLE), which would keep the spans under a VBL (done: the next section); the first frame and the ~11 ms a
frame takes the guest are the TCG core's speed.

## The writeback, read back asynchronously (2026-10-18)

Where the 2.2-2.9 ms of the synchronous writeback went, timed inside it over 60-frame windows on Setup's
welcome page: binding the scratch framebuffer 5 us, `glReadPixels` 1.9-2.9 ms, the copy into the surface's
guest pages and their dirty marking 0.26-0.29 ms. With a `glFinish` in front, the finish took 0.3-1.0 ms (the
GPU still drawing the frame) and `glReadPixels` still 1.57 ms with the GPU idle: ANGLE's D3D11 staging copy
and map, not a format conversion (the texture is BGRA, and reading it as RGBA cost the same 2.05 ms).
`glReadPixels` into a pixel-pack buffer only queues the GPU copy; mapping the buffer at once cost 1.0-1.7 ms.

So the frame-end writeback of a **scanout** surface now reads into a pack buffer and maps it at the display's
next VBL (`gles_host_writeback_sync`, called by `vbl_tick` before anything is latched, so a partial or stale
frame is still never scanned out; the GL contexts are current on the vCPU thread, so it runs there through
`run_on_cpu`). By then the GPU is done and the vCPU usually idles in `wfi`. A scanout surface is one whose
first page the display latched a swapped layer from (`gles_host_note_scanout` in `front_latch`: CA's three
display surfaces); every other surface is still written back at once, since the guest CPU may read it right
after its `glFinish` (an app snapshot). The frame lands earlier when the bridge reads that memory again (a
bind or a sampled alias of the same base) or the next frame ends. CA follows each `glBindFramebuffer(0)` with
a `glFinish`; on ANGLE that is now a host `glFlush`, since every path to guest memory waits for the GPU itself
(a first build that still landed the frame there saved only 1 ms a frame). `-trace gles_call_log` prints a
`[gles-wb] T landed tex N host US` line where each frame lands; `IT_GLES_WB_SYNC=1` restores the synchronous
writeback.

The same slides as above, same unit (`m4gl`), SDL window in front, counted with the `frame-timeline` and
`gles_call_log` (`transition.py`); before = the synchronous build, 3 runs each, after = 4 runs each:

| | before | after |
|---|---|---|
| Setup push (language -> country): frames / VBLs of the slide | 16-18 / 22 | 17-19 / 22-23 |
| Setup Back (country -> language): frames / VBLs of the slide | 19-20 / 20-24 | 20-21 / 20-21 |
| host ms per slide frame (median per run) | 3.7-4.1 | 1.5-1.8 |
| the `glBindFramebuffer(0)` that ends the frame: p10 / median / p90 | 2.2 / 2.4 / 2.8 | 0.03 / 0.04 / 0.04 |
| the frame landing at the VBL, outside its span: p10 / median / p90 | - | 0.42 / 0.53 / 0.70 |
| guest span of a slide frame (> 50 draws): median / p90 / max | 11.1-14.5 / 15.7 / 18.3 ms | 8.3-8.5 / 9.0-13.6 / 15.3 ms |
| slide frames spanning more than a VBL | 2 of 70 | 0 of 102 |

Back now draws a new frame on (nearly) every VBL of its slide. The push still misses 4-5: its first frame,
which spans 3-4 VBLs while the guest builds the new page's layers, is the guest's own. The latched frames of a
push and a Back, dumped and compared (each one's horizontal shift against the previous), move in the slide's
direction throughout (a stale frame would jump back by a whole step, 20-60 px; the only reverse shift was
2 px as the Back settled), and no frame was repeated. Settings -> Safari was not measured again.

## Software against the bridge, measured (2026-10-19)

The 2026-10-18 figures above came from screendumps, which disturb what they measure. These come from
`tools/dev/n94_bench.py`: `boot` times a unit from QEMU's start to its lock screen (a screendump a second until the
slider's knob is lit), `anim` drives the home screen and counts what the panel latched in the display's own
`frame-timeline` (`/machine/display`, one entry per VBL in guest-virtual time, new = a swap) and the QEMU process's
CPU time, per gesture: six page swipes of SpringBoard, then five apps opened from their icons and closed with Home.
A launch is the tap to the last new frame of the opening (the panel quiet for 1.5 s).

Two units with the same data volume: `gl6` (Setup done, its firmware with the front end installed) and `gl6sw`, a
sparse copy of it on a copy of that firmware with `gles_frontend software` (CA_ENABLE_OGL=0 for backboardd and
SpringBoard; apps that use GL themselves, Maps, still go through the bridge). Both on the SSD (the HDD the units
usually live on was busy with another process's I/O, which made one boot 170 s), two cores, two runs each,
Radeon RX 5500 XT, `runs/n94/progress/2026-10-19_GPU_software-x-ponte.png`:

| | software (CA_ENABLE_OGL=0) | GL bridge | |
|---|---|---|---|
| boot to the lock screen | 41.1 / 37.0 s | 37.1 / 38.1 s | the same: drawing does not limit the boot |
| home screen page flick, frames/s | 21.9 / 21.7 | 60.1 / 60.1 | 2.8x: every VBL |
| frames latched per flick | 9 / 8.5 | 24.5 / 24.5 | 2.9x |
| host CPU per frame (flick) | 135 / 136 ms | 27 / 29 ms | 4.8x less |
| app opening, frames/s (5 apps) | 6.7-15.1 | 20.6-29.1 | ~2.5x |
| host CPU per frame (opening) | 139-241 ms | 38-78 ms | ~3x less |
| Ajustes, tap to settled | 4.57 / 4.42 s | 3.38 / 3.42 s | -1.1 s |
| Calendário | 2.72 / 2.10 s | 1.92 / 1.82 s | -0.5 s |
| Fotos | 2.53 / 2.47 s | 1.85 / 2.77 s | the same |
| Safari | 2.10 / 1.97 s | 1.92 s (the other run kept animating) | the same |
| Mapas | 3.55 / 3.44 s | 3.00 / 2.92 s | -0.5 s |

The swipes were first measured with the finger resting 250 ms before the lift (44 against 15-16 frames/s): the
VBLs of the rest, with nothing new to draw, counted against the animation. `n94_bench.py` now flicks (lifts while
moving, as a finger does) and the figures above are those. The idle home screen costs 15-25 % of one host core
either way. The screens are the same pixel for pixel in both
(Ajustes, Fotos, Safari, Calendário, the home screen's second page). The bridge is now the default
(`ios_prep.py prepare` / `device create`; "CoreAnimation by default" below).

## The stock apps through the bridge (2026-10-19)

Ajustes, Fotos, Safari, Calendário, Calculadora (and its scientific layout in landscape) draw as in software.
Maps drew black: it was the first guest code to use three things the bridge refused (`[gles] UNHANDLED` in the
host log), and its map is GL of its own, not CoreAnimation's:

- **Vertex array objects** (`glGenVertexArraysOES`, `glBindVertexArrayOES`, and the APPLE spelling). The bridge
  fetches vertex attributes from the guest itself at draw time (`gh.attr`), so a VAO is a saved copy of that state
  (the generic attributes' pointers, enables and buffers, and the element buffer, not ARRAY_BUFFER's binding),
  swapped in and out by the bind (`GLESVao`, `gles_vao_bind`).
- **`glProgramUniform*EXT`** (EXT_separate_shader_objects), which ANGLE's ES 3.0 context lacks: the program goes in
  use for the matching `glUniform*` and back out.
- **`#extension` after code**: Maps' fragment shaders enable `GL_OES_standard_derivatives` after their precision
  statements. Apple's compiler takes that; ANGLE holds to ESSL 1.00 ("extension directive must occur before any
  non-preprocessor tokens") and then reports the extension disabled. The bridge moves top-level `#extension` lines
  to the top (after a `#version`), leaving blank lines so the line numbers stay.

With no network Maps now shows its grid of unloaded tiles, as a phone offline does
(`runs/n94/progress/2026-10-19_GPU_apps-pela-ponte.png`). Left: one `glDrawElements` at boot returns
GL_INVALID_ENUM (`[gles] REFUSED draw-error:0x500`, before the first frame; nothing visible is missing).

## CoreAnimation by default

`ios_prep.py prepare`, and `device create` through it, give the system volume the front end (built once into the
firmware directory's `gles-frontend/` by `gles_frontend.py build`, installed with dyld's override switch) and no
`CA_ENABLE_OGL`, wherever QEMU has the bridge (on Windows, MSYS2's ANGLE DLLs in the workspace's toolchain; on
macOS, CGL). `--software-gl` keeps QuartzCore's software renderer (`CA_ENABLE_OGL=0`, `gles_frontend software`), as
does a host without the bridge. Each run is idempotent (the build is reproducible: an install over the same bytes is
"already in place"). A firmware directory that was in software changes its system volume the first time: units
already made from it need `device refresh` (the FTL's copies of served pages do not follow an edit). Checked
2026-10-19: `device create` of a new 4 GiB unit from the IPSW on the shared `firmware/n94_10B329`, first boot to
Setup's welcome page in 71 s on the SSD, `[gles] CoreAnimation composites through the host` in its log.

## Limitations

- Windows only with Direct3D 11 (ANGLE's default here); the Vulkan and SwiftShader backends in the package are not
  selected.
- No GL snapshots (savevm/migration) while a guest GL context is live.
- `gles_frontend install` edits a system volume image every unit of that firmware shares; a unit that should stay
  in software needs its own firmware directory (`prepare --software-gl`).
