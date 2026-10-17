"""Apple's flattened device tree (iBoot's DeviceTree.img3 payload): parsed, edited in place, serialized.

Node = u32 nprops, u32 nchildren, props {char name[32]; u32 length (bit 31 set: a placeholder iBoot
fills); value padded to 4}, then the children. Paths join the nodes' "name" properties below the root
with "/" ("" is the root, "arm-io/uart0"); a later node with the same path shadows an earlier one.
set() writes into the existing slot and never resizes; add()/add_node() grow the blob and remove()
shrinks it, so lay memory out after the last one. (Layout after FirmwareKit's DeviceTree.swift /
qemu-ios ipad1_kboot.py.)
"""
import struct


def le(words):
    return struct.pack("<%dI" % len(words), *words)


def name32(s):
    b = s.encode()[:32]
    return b + b"\0" * (32 - len(b))


def encode(value):
    if isinstance(value, (bytes, bytearray)):
        return bytes(value)
    if isinstance(value, str):
        return value.encode() + b"\0"
    if isinstance(value, int):
        return le([value & 0xFFFFFFFF])
    return le(list(value))


def fmt(data):
    if len(data) == 0:
        return "<empty>"
    if data[0] != 0 and all(32 <= b < 127 or b == 0 for b in data):
        parts = data.rstrip(b"\0").split(b"\0")
        return " | ".join(repr(p.decode("ascii")) for p in parts)
    if len(data) % 4 == 0 and len(data) <= 64:
        words = struct.unpack_from("<%dI" % (len(data) // 4), data)
        return "{" + ", ".join("0x%x" % w for w in words) + "}"
    return "<%d bytes> %s%s" % (len(data), data[:24].hex(), "..." if len(data) > 24 else "")


class DeviceTree:
    def __init__(self, blob):
        self.data = bytearray(blob)
        self._reparse()

    # ---- queries -------------------------------------------------------
    def contains(self, path):
        return path in self.props

    def has(self, path, prop):
        return self.props.get(path, {}).get(prop) is not None

    def slot_len(self, path, prop):
        s = self.props.get(path, {}).get(prop)
        return s[1] if s else None

    def value(self, path, prop):
        s = self.props.get(path, {}).get(prop)
        if s is None:
            return None
        off, length = s
        return bytes(self.data[off + 36:off + 36 + length])

    def string(self, path, prop):
        v = self.value(path, prop)
        return v.split(b"\0")[0].decode("ascii", "replace") if v is not None else None

    def strings(self, path, prop):
        v = self.value(path, prop)
        return [p.decode("ascii", "replace") for p in v.rstrip(b"\0").split(b"\0")] if v else []

    def u32s(self, path, prop):
        v = self.value(path, prop)
        return list(struct.unpack_from("<%dI" % (len(v) // 4), v)) if v else []

    def children(self, path):
        prefix = path + "/" if path else ""
        return [p for p in self.order if p.startswith(prefix) and "/" not in p[len(prefix):] and p != path]

    # ---- edits ---------------------------------------------------------
    def set(self, path, prop, value):
        """value: bytes, int (u32), str (NUL-terminated) or a list of u32s."""
        s = self.props.get(path, {}).get(prop)
        if s is None:
            raise KeyError(f"DeviceTree: no {path}:{prop}")
        v = encode(value)
        off, length = s
        if len(v) > length:
            raise ValueError(f"DeviceTree: {path}:{prop} holds {length} bytes, got {len(v)}")
        self.data[off + 36:off + 36 + length] = v + b"\0" * (length - len(v))

    def set_if(self, path, prop, value):
        if self.has(path, prop):
            self.set(path, prop, value)
            return True
        return False

    def add(self, path, prop, value=b""):
        node = self.nodes.get(path)
        if node is None:
            raise KeyError(f"DeviceTree: no node {path}")
        v = encode(value)
        rec = name32(prop) + le([len(v)]) + v + b"\0" * (((len(v) + 3) & ~3) - len(v))
        start, props_end = node
        self.data[props_end:props_end] = rec
        n = struct.unpack_from("<I", self.data, start)[0]
        self.data[start:start + 4] = le([n + 1])
        self._reparse()

    def add_node(self, parent, name, props=()):
        node = self.nodes.get(parent)
        end = self.ends.get(parent)
        if node is None:
            raise KeyError(f"DeviceTree: no node {parent}")
        items = [("name", name)] + list(props)
        rec = le([len(items), 0])
        for k, v in items:
            e = encode(v)
            rec += name32(k) + le([len(e)]) + e + b"\0" * (((len(e) + 3) & ~3) - len(e))
        self.data[end:end] = rec
        start = node[0]
        n = struct.unpack_from("<I", self.data, start + 4)[0]
        self.data[start + 4:start + 8] = le([n + 1])
        self._reparse()

    def remove(self, path, prop):
        """Drop a property, record and all (a placeholder iBoot would fill is worse than none: IODTNVRAM
        walks an all-zero nvram-proxy-data as partitions of length 0 and never ends)."""
        s = self.props.get(path, {}).get(prop)
        if s is None:
            raise KeyError(f"DeviceTree: no {path}:{prop}")
        off, length = s
        start = self.nodes[path][0]
        del self.data[off:off + 36 + ((length + 3) & ~3)]
        n = struct.unpack_from("<I", self.data, start)[0]
        self.data[start:start + 4] = le([n - 1])
        self._reparse()

    def rename(self, path, old, new):
        s = self.props.get(path, {}).pop(old, None)
        if s is None:
            raise KeyError(f"DeviceTree: no {path}:{old}")
        self.data[s[0]:s[0] + 32] = name32(new)
        self.props.setdefault(path, {})[new] = s

    def unmatch(self, path, new_name=None):
        """Hide a node from IOKit matching: compatible/device_type -> "none" (or "off" / "x" when the
        slot is shorter, as a 4-byte device_type "cpu" is), and its name, if given."""
        for k in ("compatible", "device_type"):
            n = self.slot_len(path, k)
            if n:
                self.set(path, k, "none" if n >= 5 else "off" if n >= 4 else "x")
        if new_name:
            self.set(path, "name", new_name)

    # ---- parsing -------------------------------------------------------
    def _u32(self, at):
        return struct.unpack_from("<I", self.data, at)[0]

    def _reparse(self):
        self.props, self.nodes, self.ends, self.order = {}, {}, {}, []
        self.records = []   # every node record in tree order, shadowed duplicates included
        end = self._node(0, None)
        if end != len(self.data):
            raise ValueError("DeviceTree: trailing bytes after the device tree")

    def _node(self, start, parent):
        if start + 8 > len(self.data):
            raise ValueError("DeviceTree: truncated")
        nprops, nchildren = self._u32(start), self._u32(start + 4)
        off = start + 8
        mine = {}
        for _ in range(nprops):
            if off + 36 > len(self.data):
                raise ValueError("DeviceTree: truncated")
            name = bytes(self.data[off:off + 32]).split(b"\0")[0].decode("ascii", "replace")
            length = self._u32(off + 32) & 0x7FFFFFFF
            if off + 36 + length > len(self.data):
                raise ValueError("DeviceTree: truncated")
            mine[name] = (off, length)
            off += 36 + ((length + 3) & ~3)
        if "name" not in mine:
            raise ValueError("DeviceTree: node without a name")
        n = mine["name"]
        name = bytes(self.data[n[0] + 36:n[0] + 36 + n[1]]).split(b"\0")[0].decode("ascii", "replace")
        path = "" if parent is None else (f"{parent}/{name}").lstrip("/")
        self.props[path] = mine
        self.nodes[path] = (start, off)
        self.order.append(path)
        self.records.append((path, mine))
        for _ in range(nchildren):
            off = self._node(off, path)
        self.ends[path] = off
        return off

    # ---- dump ----------------------------------------------------------
    def dump(self, out):
        """Every node and property, in tree order, readable."""
        for path, props in self.records:
            out.write(("/" + path if path else "/") + "\n")
            for k, (off, length) in props.items():
                if k == "name":
                    continue
                v = bytes(self.data[off + 36:off + 36 + length])
                out.write("    %-30s %s\n" % (k, fmt(v)))
