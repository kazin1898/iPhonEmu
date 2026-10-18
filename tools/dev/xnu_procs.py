#!/usr/bin/env python3
"""List the guest's processes through QEMU's gdbstub, with no debugger: the GDB remote protocol
spoken directly, the kernel's allproc list walked from its exported symbols.

  xnu_procs.py KERNELCACHE [--port 1234] [--samples N]

For the kboot boards (no KASLR, the kernelcache at its file addresses). It halts the guest, waits
for a stop in kernel mode (kernel addresses only read through TTBR1 there), reads the list,
resumes. allproc is not exported by the 6.x kernels: it is found from kernproc, the list's last
entry, by following p_list.le_prev back to the first link outside the heap (the list head in
__DATA), as imgtools/lldb/xnu.py does for 7B500. proc->p_pid and proc->task come from the
exported proc_pid / proc_task (one Thumb ldr each), p_comm from where kernproc holds "kernel_task".
Kept small on purpose: the process list is what the M3 bring-up needed (which daemons run).
"""
import argparse
import socket
import struct
import time


class Rsp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.buf = b""

    def _send(self, payload):
        data = payload.encode()
        self.s.sendall(b"$%s#%02x" % (data, sum(data) & 0xff))

    def _recv_packet(self):
        while True:
            i = self.buf.find(b"$")
            j = self.buf.find(b"#", i + 1) if i >= 0 else -1
            if i >= 0 and j >= 0 and len(self.buf) >= j + 3:
                pkt = self.buf[i + 1:j]
                self.buf = self.buf[j + 3:]
                self.s.sendall(b"+")
                return pkt.decode()
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk

    def cmd(self, payload):
        self._send(payload)
        while True:
            p = self._recv_packet()
            if p:
                return p

    def halt(self):
        self.s.sendall(b"\x03")
        return self._recv_packet()

    def cont(self):
        self._send("c")

    def regs(self):
        g = bytes.fromhex(self.cmd("g"))
        return struct.unpack_from("<16I", g)

    def mem(self, addr, n):
        out = b""
        while n:
            k = min(n, 0x400)
            r = self.cmd("m%x,%x" % (addr, k))
            if r.startswith("E"):
                raise IOError("read %08x: %s" % (addr, r))
            out += bytes.fromhex(r)
            addr += k
            n -= k
        return out

    def u32(self, addr):
        return struct.unpack("<I", self.mem(addr, 4))[0]


def data_segment(path):
    """The kernel's own __DATA [start, end): where allproc's head lives."""
    d = open(path, "rb").read()
    ncmds, p = struct.unpack_from("<I", d, 16)[0], 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", d, p)
        if cmd == 1 and d[p + 8:p + 24].rstrip(b"\0") == b"__DATA":
            vm, vmsize = struct.unpack_from("<II", d, p + 24)
            return vm, vm + vmsize
        p += size
    raise ValueError("no __DATA")


def kernel_symbols(path, names):
    d = open(path, "rb").read()
    ncmds, p, out = struct.unpack_from("<I", d, 16)[0], 28, {}
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", d, p)
        if cmd == 2:
            so, ns, stro, _ = struct.unpack_from("<IIII", d, p + 8)
            for k in range(ns):
                strx, _, _, _, val = struct.unpack_from("<IBBhI", d, so + 12 * k)
                n = d[stro + strx:d.index(b"\0", stro + strx)].decode()
                if n.lstrip("_") in names:
                    out[n.lstrip("_")] = val
        p += size
    return out


def thumb_ldr_imm(code):
    """The immediate of the first `ldr rX, [r0, #imm]` (16- or 32-bit Thumb) in a short accessor."""
    for i in range(0, len(code) - 2, 2):
        h = struct.unpack_from("<H", code, i)[0]
        if h >> 11 == 0b01101 and (h >> 3) & 7 == 0:            # LDR (imm5 * 4), Rn = r0
            return ((h >> 6) & 0x1f) * 4
        if h == 0xf8d0 and i + 4 <= len(code):                   # LDR.W rt, [r0, #imm12]
            return struct.unpack_from("<H", code, i + 2)[0] & 0xfff
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("kernel")
    ap.add_argument("--port", type=int, default=1234)
    args = ap.parse_args()

    sym = kernel_symbols(args.kernel, {"kernproc", "kernel_task", "proc_pid", "proc_task"})
    data = data_segment(args.kernel)
    r = Rsp(args.port)
    r.cmd("?")                                # QEMU stops the guest when a debugger connects
    for _ in range(200):                     # a stop in kernel mode: TTBR1 reads only work there
        if r.regs()[15] >= 0x80000000:
            break
        r.cont()
        time.sleep(0.05)
        r.halt()
    try:
        pid_off = thumb_ldr_imm(r.mem(sym["proc_pid"] & ~1, 16))
        task_off = thumb_ldr_imm(r.mem(sym["proc_task"] & ~1, 16))
        kp = r.u32(sym["kernproc"])
        blob = r.mem(kp, 0x200)
        comm_off = blob.find(b"kernel_task")
        # Back along le_prev to the list head: the first link that is not a proc in the heap.
        cur, head = kp, None
        for _ in range(4096):
            prev = r.u32(cur + 4)               # &previous->le_next, or &allproc.lh_first
            if data[0] <= prev < data[1]:
                head = prev
                break
            cur = prev
        rows = []
        p = r.u32(head)
        while p and len(rows) < 2048:
            pid = r.u32(p + pid_off)
            name = r.mem(p + comm_off, 17).split(b"\0")[0].decode(errors="replace")
            rows.append((pid, name, p, r.u32(p + task_off)))
            p = r.u32(p)
        print("pid_off %#x task_off %#x comm_off %#x allproc %#x" % (pid_off, task_off, comm_off, head))
        for pid, name, p, task in sorted(rows):
            print("%5d %-17s proc %08x task %08x" % (pid, name, p, task))
    finally:
        print("detach:", r.cmd("D"))             # resumes the guest


if __name__ == "__main__":
    main()
