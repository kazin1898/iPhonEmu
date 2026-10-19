#!/usr/bin/env python3
"""A usbmuxd for the emulated device: the host side of the tcp_usb transport (docs/tcp-usb-protocol.md)
and the usbmuxd client protocol, so libimobiledevice's tools (ideviceinfo, idevicepair, afcclient...)
reach the guest's lockdownd and services on a host with no usbmuxd (Windows).

  usbmux_bridge.py [--usb 127.0.0.1:1235] [--mux 127.0.0.1:27015] [--records DIR] [-v]

QEMU dials --usb (the machine's usb-tcp-addr, or IT_USB_TCP); clients dial --mux, the address
libusbmuxd uses on Windows (USBMUXD_SOCKET_ADDRESS=host:port elsewhere). The bridge enumerates the
device by hand as a host port would (descriptors, SET_ADDRESS, the configuration with the mux interface
255/254/2, SET_CONFIGURATION, Apple's vendor power request for 500 + 1600 mA, the serial string = the
UDID), then speaks the device's mux protocol on that interface's bulk pipes: the version exchange, the
v2 header, and a TCP per client connection, as usbmuxd's device.c does. Pair records and the host's BUID
are kept in --records (default ~/.iphonemu/lockdown), one plist per UDID.

Written for the iPhonEmu machines; one device at a time.
"""
import argparse
import os
import plistlib
import queue
import select
import socket
import struct
import sys
import threading
import time
import uuid

HDR = struct.Struct("<BBBh")
F_SETUP, F_RESET, F_ENUMDONE, F_HELLO = 1, 2, 4, 8
USB_DIR_IN = 0x80
NAK, STALL = -2, -3
TUSB_MAGIC = 0x42535554

MUX_VERSION, MUX_CONTROL, MUX_SETUP, MUX_TCP = 0, 1, 2, 6
MUX_MAGIC = 0xFEEDFACE
TH_FIN, TH_SYN, TH_RST, TH_PUSH, TH_ACK = 1, 2, 4, 8, 0x10
RX_WIN = 131072                      # what the host side offers (in 256-byte units on the wire)
DEVICE_ID = 1

VERBOSE = False


def log(*a):
    print("[bridge]", *a, flush=True)


def vlog(*a):
    if VERBOSE:
        log(*a)


class UsbLink:
    """The tcp_usb transport: strictly host-driven request/response over QEMU's socket."""

    def __init__(self, sock):
        self.s = sock
        self.max_xact = 16384

    def _recv(self, n):
        buf = b""
        while len(buf) < n:
            c = self.s.recv(n - len(buf))
            if not c:
                raise ConnectionError("device closed the link")
            buf += c
        return buf

    def xact(self, ep, flags=0, length=0, payload=b""):
        """One transaction: (reply length or USB_RET_* code, IN data)."""
        self.s.sendall(HDR.pack(0, ep, flags, length) + (payload if not ep & USB_DIR_IN else b""))
        _, rep, _, rlen = HDR.unpack(self._recv(HDR.size))
        data = self._recv(rlen) if rep & USB_DIR_IN and rlen > 0 else b""
        return rlen, data

    def retry(self, ep, flags=0, length=0, payload=b"", timeout=10.0):
        """Retry a transaction while the device NAKs (its only flow control)."""
        end = time.time() + timeout
        while True:
            r, d = self.xact(ep, flags, length, payload)
            if r != NAK:
                return r, d
            if time.time() > end:
                raise TimeoutError("ep 0x%02x kept NAKing" % ep)
            time.sleep(0.005)

    def control_in(self, rtype, req, val, idx, length):
        setup = struct.pack("<BBHHH", rtype, req, val, idx, length)
        r, _ = self.retry(0, F_SETUP, 8, setup)
        if r < 0:
            raise IOError("SETUP %02x/%02x: %d" % (rtype, req, r))
        data = b""
        while len(data) < length:
            r, d = self.retry(USB_DIR_IN, 0, length - len(data))
            if r < 0:
                raise IOError("control IN %02x/%02x: %d" % (rtype, req, r))
            data += d
            if r < 64:                       # a short packet ends the data stage
                break
        r, _ = self.retry(0, 0, 0)           # status: zero-length OUT
        return data

    def control_out(self, rtype, req, val, idx):
        setup = struct.pack("<BBHHH", rtype, req, val, idx, 0)
        r, _ = self.retry(0, F_SETUP, 8, setup)
        if r < 0:
            raise IOError("SETUP %02x/%02x: %d" % (rtype, req, r))
        r, _ = self.retry(USB_DIR_IN, 0, 0)  # status: zero-length IN
        if r < 0:
            raise IOError("status %02x/%02x: %d" % (rtype, req, r))


def mux_interface(cfg):
    """(bulk IN, bulk OUT, max packet) of the mux interface (255/254/2) in a configuration descriptor."""
    i, inmux, ep_in, ep_out, mps = 0, False, None, None, 512
    while i + 1 < len(cfg) and cfg[i] >= 2:
        if cfg[i + 1] == 4 and i + 8 < len(cfg):
            inmux = cfg[i + 5:i + 8] == bytes([255, 254, 2])
        elif cfg[i + 1] == 5 and inmux:
            ep = cfg[i + 2]
            mps = struct.unpack_from("<H", cfg, i + 4)[0]
            if ep & USB_DIR_IN:
                ep_in = ep
            else:
                ep_out = ep
        i += cfg[i]
    return (ep_in, ep_out, mps) if ep_in and ep_out else None


def enumerate_device(link, boot_wait):
    """Hello, reset and enumeration; returns a dict with the UDID and the mux pipes."""
    r, d = link.xact(0x7F | USB_DIR_IN, F_HELLO, 12)
    vlog("hello ->", r, d.hex())
    if r >= 12:
        magic, ver, _, maxx = struct.unpack("<IHHI", d[:12])
        if magic != TUSB_MAGIC or ver != 1:
            raise IOError("tcp_usb hello: magic %#x version %d" % (magic, ver))
        link.max_xact = min(maxx, 16384)
        log("device link: tcp_usb v%d, max transaction %d" % (ver, maxx))
    t0 = time.time()
    while True:                              # the guest takes a while to bring its USB stack up
        vlog("reset ->", link.xact(0, F_RESET)[0])
        time.sleep(0.3)
        vlog("enumdone ->", link.xact(0, F_ENUMDONE)[0])
        time.sleep(0.3)
        try:
            dev = link.control_in(0x80, 6, 0x0100, 0, 18)
            vlog("device descriptor:", dev.hex())
            if len(dev) >= 18 and dev[1] == 1:
                break
        except (IOError, TimeoutError) as e:
            vlog("device descriptor:", e)
        if time.time() - t0 > boot_wait:
            raise TimeoutError("no device descriptor after %d s" % boot_wait)
        time.sleep(2)
    vid, pid = struct.unpack_from("<HH", dev, 8)
    iserial, ncfg = dev[16], dev[17]
    link.control_out(0x00, 5, 1, 0)          # SET_ADDRESS 1
    time.sleep(0.1)
    chosen = None
    for c in range(ncfg):
        hdr = link.control_in(0x80, 6, 0x0200 | c, 0, 9)
        total = struct.unpack_from("<H", hdr, 2)[0]
        cfg = link.control_in(0x80, 6, 0x0200 | c, 0, total)
        mux = mux_interface(cfg)
        if mux:
            chosen = (cfg[5], mux)
    if not chosen:
        raise IOError("no configuration has the mux interface")
    link.control_out(0x00, 9, chosen[0], 0)  # SET_CONFIGURATION
    try:
        link.control_out(0x40, 0x40, 500, 1600)   # Apple's vendor power request: a charging port
    except IOError as e:
        vlog("power request refused:", e)
    udid = ""
    if iserial:
        s = link.control_in(0x80, 6, 0x0300 | iserial, 0x0409, 255)
        udid = s[2:s[0]].decode("utf-16-le", errors="replace")
    ep_in, ep_out, mps = chosen[1]
    log("device %04x:%04x configuration %d, mux pipes in 0x%02x out 0x%02x (%d), UDID %s"
        % (vid, pid, chosen[0], ep_in, ep_out, mps, udid))
    return {"vid": vid, "pid": pid, "udid": udid, "ep_in": ep_in, "ep_out": ep_out, "mps": mps}


class MuxConn:
    def __init__(self, client, sport, dport):
        self.client = client
        self.sport, self.dport = sport, dport
        self.tx_seq = self.tx_ack = 0        # ours: next seq to send, what we have received
        self.rx_ack = 0                      # the device's acknowledgement of our bytes
        self.rx_win = 0                      # the device's window
        self.state = "syn"
        self.pending = b""                   # client bytes waiting for the device's window
        self.ack_due = False


class Bridge:
    def __init__(self, records):
        self.records = records
        self.dev = None
        self.link = None
        self.version = 0
        self.tx_seq = 0
        self.rx_seq = 0xFFFF
        self.conns = {}                      # sport -> MuxConn
        self.next_sport = 1
        self.cmds = queue.Queue()            # (kind, args) from client threads to the device thread
        self.inbuf = b""
        self.listeners = []
        self.lock = threading.Lock()
        os.makedirs(records, exist_ok=True)

    # ---- device side (one thread) ----

    def send_mux(self, proto, payload):
        if self.version >= 2:
            if proto == MUX_SETUP:           # the v2 sequence starts here, as usbmuxd's does
                self.tx_seq, self.rx_seq = 0, 0xFFFF
            hdr = struct.pack(">IIIHH", proto, 16 + len(payload), MUX_MAGIC, self.tx_seq, self.rx_seq)
        else:
            hdr = struct.pack(">II", proto, 8 + len(payload))
        self.tx_seq = (self.tx_seq + 1) & 0xFFFF
        pkt = hdr + payload
        r, _ = self.link.retry(self.dev["ep_out"], 0, len(pkt), pkt, timeout=30)
        if r < 0:
            raise IOError("bulk OUT: %d" % r)
        if len(pkt) % self.dev["mps"] == 0:  # a ZLP ends a transfer that fills its last packet
            self.link.retry(self.dev["ep_out"], 0, 0, b"", timeout=30)

    def send_tcp(self, c, flags, data=b""):
        th = struct.pack(">HHIIBBHHH", c.sport, c.dport, c.tx_seq, c.tx_ack, 5 << 4, flags,
                         RX_WIN >> 8, 0, 0)
        self.send_mux(MUX_TCP, th + data)
        c.ack_due = False

    def poll_in(self):
        """One bulk IN poll; complete mux packets go to handle_mux. True if anything came."""
        r, d = self.link.xact(self.dev["ep_in"] | USB_DIR_IN, 0, self.link.max_xact)
        if r == NAK or r <= 0:
            return False
        self.inbuf += d
        while len(self.inbuf) >= 8:
            proto, length = struct.unpack_from(">II", self.inbuf)
            if length < 8 or length > 1 << 20:
                log("mux: bad packet length %d, dropping the buffer" % length)
                self.inbuf = b""
                break
            if len(self.inbuf) < length:
                break
            pkt, self.inbuf = self.inbuf[:length], self.inbuf[length:]
            self.handle_mux(pkt)
        return True

    def handle_mux(self, pkt):
        proto, length = struct.unpack_from(">II", pkt)
        hl = 8
        if self.version >= 2 and proto != MUX_VERSION:
            magic, txs, rxs = struct.unpack_from(">IHH", pkt, 8)
            if magic not in (MUX_MAGIC, 0):      # iOS 6's AppleUSBDeviceMux sends 0
                log("mux: bad magic %#x: %s" % (magic, pkt[:48].hex()))
                return
            if proto == MUX_TCP:
                self.rx_seq = txs
            hl = 16
        body = pkt[hl:]
        vlog("mux in:", pkt[:40].hex())
        if proto == MUX_VERSION:
            major, minor = struct.unpack_from(">II", body)
            self.version = major
            log("device mux protocol %d.%d" % (major, minor))
            if self.version >= 2:
                self.send_mux(MUX_SETUP, b"\x07")
        elif proto == MUX_CONTROL:
            if body[:1] in (b"\x03", b"\x05", b"\x07"):
                vlog("device says:", body[1:].decode(errors="replace").strip())
        elif proto == MUX_TCP:
            self.handle_tcp(body)

    def handle_tcp(self, seg):
        sport, dport, seq, ack, off, flags, win = struct.unpack_from(">HHIIBBH", seg)
        data = seg[(off >> 4) * 4:]
        c = self.conns.get(dport)
        if not c:
            if not flags & TH_RST:
                vlog("tcp from %d to unknown %d" % (sport, dport))
            return
        c.rx_win = win << 8
        c.rx_ack = ack
        if flags & TH_RST:
            vlog("conn %d: reset by the device (state %s)" % (c.sport, c.state))
            if c.state == "syn":
                self.reply_result(c.client, 3, c.client.tag)   # connection refused
                c.client.refused = True
            self.close(c, rst=False)
            return
        if c.state == "syn":
            if flags & TH_SYN and flags & TH_ACK:
                c.tx_seq += 1
                c.tx_ack = seq + 1
                c.state = "open"
                self.send_tcp(c, TH_ACK)
                self.reply_result(c.client, 0, c.client.tag)
                c.client.tunnel = c
                vlog("conn %d -> device port %d open" % (c.sport, c.dport))
            return
        if data:
            c.tx_ack = (seq + len(data)) & 0xFFFFFFFF
            try:
                c.client.sock.sendall(data)
            except OSError:
                self.close(c)
                return
            c.ack_due = True
        if flags & TH_FIN:
            self.close(c)

    def close(self, c, rst=True):
        if c.sport in self.conns:
            del self.conns[c.sport]
            if rst and self.link:
                try:
                    self.send_tcp(c, TH_RST | TH_ACK)
                except (IOError, OSError, TimeoutError):
                    pass
        try:
            c.client.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    def flush_pending(self, c):
        while c.pending and c.state == "open":
            inflight = (c.tx_seq - c.rx_ack) & 0xFFFFFFFF
            room = c.rx_win - inflight
            if room <= 0:
                return
            n = min(len(c.pending), room, self.link.max_xact - 48)
            chunk, c.pending = c.pending[:n], c.pending[n:]
            self.send_tcp(c, TH_ACK, chunk)
            c.tx_seq = (c.tx_seq + n) & 0xFFFFFFFF

    def device_loop(self, sock, boot_wait):
        self.link = UsbLink(sock)
        self.dev = enumerate_device(self.link, boot_wait)
        self.version, self.tx_seq, self.rx_seq, self.inbuf = 0, 0, 0xFFFF, b""
        self.send_mux(MUX_VERSION, struct.pack(">III", 2, 0, 0))
        t0 = time.time()
        while self.version == 0:
            if not self.poll_in():
                time.sleep(0.01)
            if time.time() - t0 > 30:
                raise TimeoutError("no mux version reply")
        self.notify_attached()
        idle = 0.0
        while True:
            busy = False
            while not self.cmds.empty():
                kind, arg = self.cmds.get()
                busy = True
                if kind == "connect":
                    client, port = arg
                    c = MuxConn(client, self.next_sport, port)
                    self.next_sport = self.next_sport % 0xFFFF + 1
                    self.conns[c.sport] = c
                    self.send_tcp(c, TH_SYN)
                elif kind == "data":
                    c, data = arg
                    if c.sport in self.conns:
                        c.pending += data
                elif kind == "closed":
                    if arg.sport in self.conns:
                        self.close(arg)
            for c in list(self.conns.values()):
                if c.pending:
                    self.flush_pending(c)
                    busy = True
            if self.poll_in():
                busy = True
            for c in list(self.conns.values()):
                if c.ack_due:
                    self.send_tcp(c, TH_ACK)
            idle = 0 if busy else min(idle + 0.001, 0.01)
            if idle:
                time.sleep(idle)

    # ---- usbmuxd client protocol ----

    def notify_attached(self):
        for cl in list(self.listeners):
            self.send_plist(cl, self.attached_plist(), 0)

    def attached_plist(self):
        return {"MessageType": "Attached", "DeviceID": DEVICE_ID, "Properties": self.dev_props()}

    def dev_props(self):
        return {"ConnectionSpeed": 480000000, "ConnectionType": "USB", "DeviceID": DEVICE_ID,
                "LocationID": 0, "ProductID": self.dev["pid"], "SerialNumber": self.dev["udid"]}

    def send_plist(self, cl, d, tag):
        body = plistlib.dumps(d)
        try:
            cl.sock.sendall(struct.pack("<IIII", 16 + len(body), 1, 8, tag) + body)
        except OSError:
            pass

    def reply_result(self, cl, num, tag):
        self.send_plist(cl, {"MessageType": "Result", "Number": num}, tag)

    def record_path(self, name):
        return os.path.join(self.records, "".join(ch for ch in name if ch.isalnum() or ch in "-_") + ".plist")

    def buid(self):
        p = self.record_path("SystemConfiguration")
        if os.path.exists(p):
            with open(p, "rb") as f:
                return plistlib.load(f)["SystemBUID"]
        b = str(uuid.uuid4()).upper()
        with open(p, "wb") as f:
            plistlib.dump({"SystemBUID": b}, f)
        return b

    def client_thread(self, sock):
        cl = type("Client", (), {})()
        cl.sock, cl.tunnel, cl.tag = sock, None, 0
        try:
            while cl.tunnel is None:
                hdr = b""
                while len(hdr) < 16:
                    c = sock.recv(16 - len(hdr))
                    if not c:
                        return
                    hdr += c
                length, version, msg, tag = struct.unpack("<IIII", hdr)
                body = b""
                while len(body) < length - 16:
                    c = sock.recv(length - 16 - len(body))
                    if not c:
                        return
                    body += c
                cl.tag = tag
                if msg != 8:
                    self.reply_result(cl, 1, tag)
                    continue
                req = plistlib.loads(body)
                mt = req.get("MessageType")
                vlog("client:", mt)
                if mt == "ListDevices":
                    lst = [self.attached_plist()] if self.dev and self.version else []
                    self.send_plist(cl, {"DeviceList": lst}, tag)
                elif mt == "Listen":
                    self.reply_result(cl, 0, tag)
                    self.listeners.append(cl)
                    if self.dev and self.version:
                        self.send_plist(cl, self.attached_plist(), 0)
                elif mt == "ReadBUID":
                    self.send_plist(cl, {"BUID": self.buid()}, tag)
                elif mt == "ReadPairRecord":
                    p = self.record_path(req.get("PairRecordID", ""))
                    if os.path.exists(p):
                        with open(p, "rb") as f:
                            self.send_plist(cl, {"PairRecordData": f.read()}, tag)
                    else:
                        self.reply_result(cl, 2, tag)
                elif mt == "SavePairRecord":
                    with open(self.record_path(req.get("PairRecordID", "")), "wb") as f:
                        f.write(req.get("PairRecordData", b""))
                    self.reply_result(cl, 0, tag)
                elif mt == "DeletePairRecord":
                    p = self.record_path(req.get("PairRecordID", ""))
                    if os.path.exists(p):
                        os.unlink(p)
                    self.reply_result(cl, 0, tag)
                elif mt == "Connect":
                    if not self.dev or req.get("DeviceID") != DEVICE_ID:
                        self.reply_result(cl, 2, tag)
                        continue
                    pn = req.get("PortNumber", 0)
                    port = ((pn & 0xFF) << 8) | (pn >> 8)
                    vlog("client: Connect to device port %d" % port)
                    self.cmds.put(("connect", (cl, port)))
                    while cl.tunnel is None:          # the device thread replies and sets tunnel
                        r, _, _ = select.select([sock], [], [], 0.2)
                        if r and not sock.recv(1, socket.MSG_PEEK):
                            return
                        if getattr(cl, "refused", False):
                            return
                else:
                    self.reply_result(cl, 1, tag)
            c = cl.tunnel
            while True:
                data = sock.recv(65536)
                if not data:
                    break
                self.cmds.put(("data", (c, data)))
            self.cmds.put(("closed", c))
        except OSError:
            if cl.tunnel:
                self.cmds.put(("closed", cl.tunnel))
        finally:
            if cl in self.listeners:
                self.listeners.remove(cl)


def serve_clients(bridge, addr):
    host, port = addr.rsplit(":", 1)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, int(port)))
    srv.listen(16)
    log("usbmuxd protocol on %s" % addr)
    while True:
        s, _ = srv.accept()
        threading.Thread(target=bridge.client_thread, args=(s,), daemon=True).start()


def main():
    global VERBOSE
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--usb", default="127.0.0.1:1235", help="where QEMU's tcp_usb link dials in")
    ap.add_argument("--mux", default="127.0.0.1:27015", help="the usbmuxd socket clients use")
    ap.add_argument("--records", default=os.path.join(os.path.expanduser("~"), ".iphonemu", "lockdown"),
                    help="pair records and the host's BUID")
    ap.add_argument("--boot-wait", type=int, default=600, help="seconds to wait for the guest's USB stack")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    VERBOSE = a.verbose
    bridge = Bridge(a.records)
    threading.Thread(target=serve_clients, args=(bridge, a.mux), daemon=True).start()
    host, port = a.usb.rsplit(":", 1)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, int(port)))
    srv.listen(1)
    log("waiting for QEMU's tcp_usb link on %s" % a.usb)
    while True:
        s, _ = srv.accept()
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.settimeout(15)
        log("device connected")
        try:
            bridge.device_loop(s, a.boot_wait)
        except (ConnectionError, IOError, TimeoutError, OSError, socket.timeout) as e:
            log("device link ended: %s" % e)
        for c in list(bridge.conns.values()):
            bridge.close(c, rst=False)
        bridge.dev, bridge.link, bridge.version = None, None, 0
        s.close()


if __name__ == "__main__":
    main()
