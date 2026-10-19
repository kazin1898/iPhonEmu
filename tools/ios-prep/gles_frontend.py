"""The GL front end (contrib/gles-public's OpenGLES.framework) built on Windows and installed on a system volume.

iOS 6's GL goes to the host through contrib/gles-public: a replacement OpenGLES.framework/OpenGLES whose every
gl* call traps to QEMU (the cp15 QEMU_CALL register), where hw/arm/gles-host.c runs it on the host GPU (ANGLE
over Direct3D 11 on Windows: docs/windows-gl.md). build.sh makes it with ld64 and an iOS 3.x SDK on macOS; this
builds the armv7 slice iOS 6 needs with the workspace's MSYS2 clang and macho_dylib.py, no SDK:

  build    gen the stub and export tables (contrib/it-gles genstubs.sh, gles-exports.sh), compile opengles.c
           (-marm -mno-movt, contrib/gles-public/darwin-include for <pthread.h>), link with macho_dylib, and check
           the dylib exports exactly opengles.exports
  install  put it on a system volume image: OpenGLES as /System/Library/Frameworks/OpenGLES.framework/OpenGLES
           (a new file on 5.x+, where the stock one lives only in the shared cache), dyld's
           /System/Library/Caches/com.apple.dyld/enable-dylibs-to-override-cache (so dyld loads that file before
           the cached image, docs/ipad1/ios4.md) and backboardd and SpringBoard without CA_ENABLE_OGL=0, so
           CoreAnimation renders through GL again instead of in software (sysvol.UNIT_EDITS sets it for units
           without the bridge)
  software put CA_ENABLE_OGL=0 back (the files stay; with the variable set nothing renders through them)

    python -I gles_frontend.py build OUT_DIR
    python -I gles_frontend.py install ROOTFS OUT_DIR/OpenGLES
    python -I gles_frontend.py software ROOTFS
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hfs  # noqa: E402
import macho_dylib  # noqa: E402
import sysvol  # noqa: E402
from guest_install import find_tool  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
GLES_PUBLIC = os.path.join(REPO, "contrib", "gles-public")
IT_GLES = os.path.join(REPO, "contrib", "it-gles")
NAMES_H = os.path.join(REPO, "include", "hw", "arm", "guest-services", "gles-names.h")

INSTALL_NAME = "/System/Library/Frameworks/OpenGLES.framework/OpenGLES"
OVERRIDE_SWITCH = "/System/Library/Caches/com.apple.dyld/enable-dylibs-to-override-cache"
# What opengles.c binds (build.sh's text stubs name the same libraries): loaded, then searched flat.
DYLIBS = ("/usr/lib/libSystem.B.dylib", "/usr/lib/libobjc.A.dylib",
          "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
          "/System/Library/Frameworks/Foundation.framework/Foundation")
CFLAGS = ["-target", "armv7-apple-ios5.0", "-marm", "-mno-movt", "-O1", "-fno-stack-protector", "-fno-builtin",
          "-nostdinc", "-x", "objective-c", "-Wno-objc-root-class"]
GL_JOBS = ("com.apple.backboardd", "com.apple.SpringBoard")


def find_sh():
    p = shutil.which("sh")
    if p:
        return p
    cand = os.path.join(REPO, "..", "toolchain", "msys64", "usr", "bin", "sh.exe")
    if os.path.exists(cand):
        return os.path.abspath(cand)
    raise SystemExit("sh not found (the workspace's MSYS2 has one in toolchain/msys64/usr/bin)")


def build(out_dir, clang=None):
    os.makedirs(out_dir, exist_ok=True)
    sh = find_sh()
    env = dict(os.environ, PATH=os.path.dirname(sh) + os.pathsep + os.environ.get("PATH", ""))
    fwd = lambda p: os.path.abspath(p).replace("\\", "/")     # the scripts' awk takes a backslash as an escape
    subprocess.run([sh, fwd(os.path.join(IT_GLES, "genstubs.sh")), fwd(os.path.join(IT_GLES, "gles_stubs.h"))],
                   check=True, env=env)
    subprocess.run([sh, fwd(os.path.join(IT_GLES, "gles-exports.sh")), fwd(os.path.join(GLES_PUBLIC, "opengles.exports")),
                    fwd(NAMES_H), fwd(out_dir)], check=True, env=env)
    clang = find_tool("clang", clang)
    obj = os.path.join(out_dir, "opengles-armv7.o")
    subprocess.run([clang] + CFLAGS + ["-isystem", os.path.join(GLES_PUBLIC, "darwin-include"), "-I", out_dir,
                                       "-c", os.path.join(GLES_PUBLIC, "opengles.c"), "-o", obj], check=True)
    exports = [l.strip() for l in open(os.path.join(out_dir, "gles2x.exp")) if l.strip()]
    want = sorted("_" + l.strip() for l in open(os.path.join(GLES_PUBLIC, "opengles.exports"))
                  if l.strip() and not l.startswith("#"))
    if sorted(exports) != want:
        raise SystemExit("gles2x.exp is not opengles.exports")
    dylib, info = macho_dylib.link(open(obj, "rb").read(), INSTALL_NAME, exports, DYLIBS)
    out = os.path.join(out_dir, "OpenGLES")
    with open(out, "wb") as f:
        f.write(dylib)
    info.update(sha256=hashlib.sha256(dylib).hexdigest(), path=out)
    with open(os.path.join(out_dir, "OpenGLES.json"), "w") as f:
        json.dump(info, f, indent=1)
    return out, info


def install(rootfs, opengles, donor=sysvol.DONOR):
    data = open(opengles, "rb").read()
    v = hfs.Volume(rootfs, writable=True)
    try:
        need = -(-len(data) // v.block_size) + 64
        if v.free_blocks < need and donor:
            v.truncate(donor)
        try:
            rec = v.lookup(INSTALL_NAME)
        except FileNotFoundError:
            rec = None
        if rec is None:
            v.create_file(INSTALL_NAME, data, 0o755)
            what = "created"
        elif v.read_file(INSTALL_NAME) != data:
            v.set_file_data(INSTALL_NAME, data, 0o755)
            what = "replaced"
        else:
            what = "already in place"
        try:
            v.lookup(OVERRIDE_SWITCH)
            switch = "already there"
        except FileNotFoundError:
            v.create_file(OVERRIDE_SWITCH, b"", 0o644)
            switch = "created"
    finally:
        v.close()
    jobs = sysvol.edit_jobs(rootfs, GL_JOBS, console=False, unset=("CA_ENABLE_OGL",))
    return {"OpenGLES": what, "override switch": switch, "sha256": hashlib.sha256(data).hexdigest(),
            "jobs": {k: d.get("EnvironmentVariables", {}) for k, d in jobs.items()}}


def software(rootfs):
    jobs = sysvol.edit_jobs(rootfs, GL_JOBS, console=False, env=("CA_ENABLE_OGL=0",))
    return {k: d.get("EnvironmentVariables", {}) for k, d in jobs.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("build")
    p.add_argument("out_dir")
    p.add_argument("--clang")
    p = sub.add_parser("install")
    p.add_argument("rootfs")
    p.add_argument("opengles")
    p = sub.add_parser("software")
    p.add_argument("rootfs")
    a = ap.parse_args()
    if a.cmd == "build":
        path, info = build(a.out_dir, a.clang)
        print(f"{path}: {info['bytes']} bytes, {info['exports']} exports, {info['stubs']} stubs, "
              f"{info['rebases']} rebases, {info['binds']} binds; sha256 {info['sha256']}")
    elif a.cmd == "install":
        print(json.dumps(install(a.rootfs, a.opengles), indent=1))
    else:
        print(json.dumps(software(a.rootfs), indent=1))


if __name__ == "__main__":
    try:
        main()
    except macho_dylib.LinkError as e:
        sys.exit(f"gles_frontend: {e}")
