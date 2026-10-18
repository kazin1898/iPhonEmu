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
frames/s. The same within the spread: one TCG core is the limit, and every frame costs a trap per GL call and
a 640x960 readback into guest memory.

## Limitations

- Windows only with Direct3D 11 (ANGLE's default here); the Vulkan and SwiftShader backends in the package are not
  selected.
- No GL snapshots (savevm/migration) while a guest GL context is live.
- `gles_frontend install` edits a system volume image every unit of that firmware shares; a unit that should stay
  in software needs its own image or `gles_frontend software`.
