#!/usr/bin/env python3
"""Read the guest kernel's integer sysctls through QEMU's gdbstub (-gdb tcp::PORT), with no debugger:
the sysctl tree walked from the exported _sysctl__children, as `sysctl` would name the nodes.

  xnu_sysctl.py KERNELCACHE [--port 1234] [NAME ...]      (default: the hw.* CPU counts)

For the kboot boards (no KASLR). A SYSCTL_INT node's value is read through its oid_arg1 pointer.
The hw CPU counts are handler nodes (sysctl_hw_generic, which asks host_info(HOST_BASIC_INFO)): they
are read from machine_info, which is not exported; host_info loads its address from a literal, at
MACHINE_INFO_LITERAL below for the kernels it is known for (10B329: Ghidra, host_info at 0x80018b40).
There machine_info is: versions, max_cpus +8 (hw.ncpu), memory_size +0xc, max_mem +0x10, physical_cpu
+0x18, physical_cpu_max +0x1c, logical_cpu +0x20, logical_cpu_max +0x24; avail_cpus (hw.activecpu) is
a global of its own, through the next literal host_info loads.
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xnu_procs import Rsp, kernel_symbols      # noqa: E402

# struct sysctl_oid (xnu-2107, 32-bit): parent, link (next), number, kind, arg1, arg2, name, handler, fmt
OID_NEXT, OID_KIND, OID_ARG1, OID_NAME = 4, 12, 16, 24
CTLTYPE, CTLTYPE_NODE, CTLTYPE_INT = 0xf, 1, 2
# kernel tag: (literal holding &machine_info, literal holding &avail_cpus)
MACHINE_INFO_LITERAL = {"RELEASE_ARM_S5L8940X": (0x802d01a8, 0x802d01b4)}  # xnu-2107.7.55.2.2, 6.1.3 10B329
MACHINE_INFO = {"hw.ncpu": 8, "hw.physicalcpu": 0x18, "hw.physicalcpu_max": 0x1c,
                "hw.logicalcpu": 0x20, "hw.logicalcpu_max": 0x24}
DEFAULT = ["hw.ncpu", "hw.activecpu", "hw.physicalcpu", "hw.physicalcpu_max", "hw.logicalcpu",
           "hw.logicalcpu_max"]


def cstr(r, addr, n=64):
    b = r.mem(addr, n)
    return b.split(b"\0")[0].decode(errors="replace")


def children(r, head):
    """(name, oid address) of every node in the list whose head is at `head`."""
    oid = r.u32(head)
    while oid:
        yield cstr(r, r.u32(oid + OID_NAME)), oid
        oid = r.u32(oid + OID_NEXT)


def lookup(r, root, name):
    head = root
    oid = None
    for part in name.split("."):
        oid = dict(children(r, head)).get(part)
        if oid is None:
            return None
        if r.u32(oid + OID_KIND) & CTLTYPE == CTLTYPE_NODE:
            head = r.u32(oid + OID_ARG1)
    return oid


def machine_info_literal(kernel):
    """host_info's literals (&machine_info, &avail_cpus), for a kernel MACHINE_INFO_LITERAL knows."""
    d = open(kernel, "rb").read()
    for tag, va in MACHINE_INFO_LITERAL.items():
        if b"xnu-2107.7.55.2.2~1/" + tag.encode() in d:
            return va
    return None


def read_int(r, root, name, literals=None):
    if name in MACHINE_INFO:
        return r.u32(r.u32(literals[0]) + MACHINE_INFO[name]) if literals else None
    if name == "hw.activecpu":
        return r.u32(r.u32(literals[1])) if literals else None
    oid = lookup(r, root, name)
    if oid is None or r.u32(oid + OID_KIND) & CTLTYPE != CTLTYPE_INT or not r.u32(oid + OID_ARG1):
        return None
    return r.u32(r.u32(oid + OID_ARG1))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("kernel")
    ap.add_argument("names", nargs="*")
    ap.add_argument("--port", type=int, default=1234)
    args = ap.parse_args()

    root = kernel_symbols(args.kernel, {"sysctl__children"})["sysctl__children"]
    literal = machine_info_literal(args.kernel)
    r = Rsp(args.port)
    r.cmd("?")                                # QEMU stops the guest when a debugger connects
    for _ in range(200):                     # a stop in kernel mode: TTBR1 reads only work there
        if r.regs()[15] >= 0x80000000:
            break
        r.cont()
        time.sleep(0.05)
        r.halt()
    try:
        for name in args.names or DEFAULT:
            v = read_int(r, root, name, literal)
            print("%s: %s" % (name, "?" if v is None else v))
    finally:
        r.cont()


if __name__ == "__main__":
    main()
