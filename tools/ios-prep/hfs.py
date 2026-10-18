"""HFS+/HFSX volumes, read and edited in place: what ios-prep needs to stage files in the IPSW's images.

Reads any file of a volume, following hard links ('hlnk' records into the private-data folder) and
HFS compression (UF_COMPRESSED + the com.apple.decmpfs attribute, inline or in the resource fork, zlib
or stored). Edits only in place, so no B-tree is ever restructured (a new record goes into a leaf with room):

- set_file_data(path, data) gives an existing file a new, uncompressed data fork: the bytes go in
  freshly allocated contiguous blocks, the catalog record's data fork is rewritten in place and its
  UF_COMPRESSED flag cleared (the decmpfs attribute stays behind, unread);
- create_file(path, data) adds a file to an existing folder (a catalog record inserted into the leaf node
  its key sorts into, when that node has room);
- grow(total_blocks) adds free space at the end, up to what the allocation file's bitmap can describe,
  and moves the alternate volume header.

Journaled volumes are taken as they come from Apple, with an empty journal; nothing here writes one.
"""
import os
import struct
import zlib

UF_COMPRESSED = 0x20
ROOT_FOLDER_ID = 2
PRIVATE_DATA = "\0\0\0\0HFS+ Private Data"
REC_FOLDER, REC_FILE = 1, 2


def u16(b, o): return struct.unpack_from(">H", b, o)[0]
def u32(b, o): return struct.unpack_from(">I", b, o)[0]
def u64(b, o): return struct.unpack_from(">Q", b, o)[0]


class Fork:
    """HFSPlusForkData (80 bytes)."""

    def __init__(self, buf, off):
        self.logical = u64(buf, off)
        self.clump = u32(buf, off + 8)
        self.total_blocks = u32(buf, off + 12)
        self.extents = [struct.unpack_from(">II", buf, off + 16 + 8 * i) for i in range(8)]
        self.extents = [(s, c) for s, c in self.extents if c]

    @staticmethod
    def pack(logical, extents, clump=0):
        out = struct.pack(">QII", logical, clump, sum(c for _, c in extents))
        for s, c in (list(extents) + [(0, 0)] * 8)[:8]:
            out += struct.pack(">II", s, c)
        return out


class FileRecord:
    """A catalog file record and where it lives in the catalog fork."""

    def __init__(self, vol, at, data_off, raw, parent, name):
        self.vol, self.at, self.data_off, self.parent, self.name = vol, at, data_off, parent, name
        r = raw[data_off:]
        self.file_id = u32(r, 8)
        self.owner_flags = r[41]
        self.mode = u16(r, 42)
        self.special = u32(r, 44)
        self.type, self.creator = r[48:52], r[52:56]
        self.data = Fork(r, 88)
        self.rsrc = Fork(r, 168)

    @property
    def compressed(self):
        return bool(self.owner_flags & UF_COMPRESSED)

    @property
    def hard_link(self):
        return self.type == b"hlnk" and self.creator == b"hfs+"


class Volume:
    def __init__(self, path, writable=False):
        self.path = path
        self.f = open(path, "r+b" if writable else "rb")
        self.f.seek(1024)
        self.vh = bytearray(self.f.read(512))
        if self.vh[0:2] not in (b"H+", b"HX"):
            raise ValueError(f"{path}: not HFS+/HFSX (signature {bytes(self.vh[0:2])!r})")
        self.block_size = u32(self.vh, 40)
        self.total_blocks = u32(self.vh, 44)
        self.free_blocks = u32(self.vh, 48)
        self.alloc = Fork(self.vh, 112)
        self.catalog_fork = Fork(self.vh, 272)
        self.attributes_fork = Fork(self.vh, 352)
        for name, fork in (("catalog", self.catalog_fork), ("attributes", self.attributes_fork),
                           ("allocation", self.alloc)):
            if sum(c for _, c in fork.extents) != fork.total_blocks:
                raise ValueError(f"{path}: the {name} file spills into the extents overflow file")
        self.catalog = bytearray(self.read_fork(self.catalog_fork))
        self.cat_node = u16(self.catalog, 14 + 18)
        self._index = None
        self._attrs = None

    def close(self):
        self.f.close()

    # ---- raw I/O --------------------------------------------------------------------------------------
    def read_blocks(self, start, count):
        self.f.seek(start * self.block_size)
        data = self.f.read(count * self.block_size)
        return data + bytes(count * self.block_size - len(data))

    def read_fork(self, fork, length=None):
        want = fork.logical if length is None else length
        out = bytearray()
        for start, count in fork.extents:
            if len(out) >= want:
                break
            out += self.read_blocks(start, count)
        return bytes(out[:want])

    def fork_offset(self, fork, off):
        """The image offset of byte `off` of a fork."""
        for start, count in fork.extents:
            n = count * self.block_size
            if off < n:
                return start * self.block_size + off
            off -= n
        raise ValueError("offset past the fork's extents")

    # ---- B-trees --------------------------------------------------------------------------------------
    @staticmethod
    def leaf_records(tree):
        """(node index, record offset in the tree, record bytes) for every leaf record, in key order."""
        node_size = u16(tree, 14 + 18)
        idx = u32(tree, 14 + 10)
        seen = 0
        while idx and seen <= len(tree) // node_size:
            seen += 1
            base = idx * node_size
            node = tree[base:base + node_size]
            if len(node) < node_size or node[8] != 0xFF:
                break
            n = u16(node, 10)
            for i in range(n):
                off = u16(node, node_size - 2 * (i + 1))
                end = u16(node, node_size - 2 * (i + 2))
                yield idx, base + off, bytes(node[off:end])
            idx = u32(node, 0)

    def index(self):
        """{(parent id, name): (offset in the catalog, data offset, record type)} over the catalog's leaves."""
        if self._index is None:
            self._index = {}
            for _, at, rec in self.leaf_records(self.catalog):
                key_len = u16(rec, 0)
                parent = u32(rec, 2)
                name = rec[8:8 + 2 * u16(rec, 6)].decode("utf-16-be")
                data_off = 2 + key_len
                rtype = u16(rec, data_off)
                if rtype in (REC_FOLDER, REC_FILE):
                    self._index[(parent, name)] = (at, data_off, rtype, len(rec))
        return self._index

    def lookup(self, path):
        """The FileRecord at `path` (absolute, '/'-separated), or the folder ID for a folder."""
        parent = ROOT_FOLDER_ID
        parts = [p for p in path.split("/") if p]
        for i, part in enumerate(parts):
            hit = self.index().get((parent, part))
            if not hit:
                raise FileNotFoundError(f"{self.path}: {'/' + '/'.join(parts[:i + 1])}")
            at, data_off, rtype, rlen = hit
            rec = bytes(self.catalog[at:at + rlen])
            if rtype == REC_FOLDER:
                parent = u32(rec, data_off + 8)
                if i == len(parts) - 1:
                    return parent
            elif i == len(parts) - 1:
                return FileRecord(self, at, data_off, rec, parent, part)
            else:
                raise NotADirectoryError(path)
        return parent

    def resolve(self, rec):
        """A hard link's inode record, or the record itself."""
        if isinstance(rec, FileRecord) and rec.hard_link:
            priv = self.index().get((ROOT_FOLDER_ID, PRIVATE_DATA))
            if not priv:
                raise FileNotFoundError("hard link with no private-data folder")
            pat, pdo, _, plen = priv
            pid = u32(self.catalog[pat:pat + plen], pdo + 8)
            name = f"iNode{rec.special}"
            at, data_off, _, rlen = self.index()[(pid, name)]
            return FileRecord(self, at, data_off, bytes(self.catalog[at:at + rlen]), pid, name)
        return rec

    def attributes(self):
        """{(file id, name): record bytes} from the attributes B-tree (inline records only)."""
        if self._attrs is None:
            self._attrs = {}
            if self.attributes_fork.logical:
                tree = self.read_fork(self.attributes_fork)
                for _, _, rec in self.leaf_records(tree):
                    key_len = u16(rec, 0)
                    fid = u32(rec, 4)
                    name = rec[14:14 + 2 * u16(rec, 12)].decode("utf-16-be", "replace")
                    self._attrs[(fid, name)] = rec[2 + key_len:]
        return self._attrs

    # ---- file contents --------------------------------------------------------------------------------
    def read_file(self, path):
        rec = self.resolve(self.lookup(path))
        if not isinstance(rec, FileRecord):
            raise IsADirectoryError(path)
        if not rec.compressed:
            return self.read_fork(rec.data)
        attr = self.attributes().get((rec.file_id, "com.apple.decmpfs"))
        if not attr or u32(attr, 0) != 0x10:
            raise ValueError(f"{path}: compressed but no inline decmpfs attribute")
        blob = attr[16:16 + u32(attr, 12)]
        magic, ctype, size = struct.unpack_from("<4sIQ", blob, 0)
        if magic != b"fpmc":
            raise ValueError(f"{path}: bad decmpfs header")
        if ctype == 3:
            body = blob[16:]
            return body[1:size + 1] if body[:1] == b"\xff" else zlib.decompress(body)[:size]
        if ctype == 4:
            rsrc = self.read_fork(rec.rsrc)
            base = u32(rsrc, 0) + 4
            nblocks = struct.unpack_from("<I", rsrc, base)[0]
            out = bytearray()
            for i in range(nblocks):
                off, n = struct.unpack_from("<II", rsrc, base + 4 + 8 * i)
                chunk = rsrc[base + off:base + off + n]
                out += chunk[1:] if chunk[:1] == b"\xff" else zlib.decompress(chunk)
            return bytes(out[:size])
        raise ValueError(f"{path}: decmpfs type {ctype} is not supported")

    # ---- allocation -----------------------------------------------------------------------------------
    def bitmap(self):
        return bytearray(self.read_fork(self.alloc))

    def write_bitmap(self, bm):
        off = 0
        for start, count in self.alloc.extents:
            n = count * self.block_size
            self.f.seek(start * self.block_size)
            self.f.write(bytes(bm[off:off + n]).ljust(n, b"\0"))
            off += n

    def allocated(self, bm=None):
        """Every allocated block, as sorted (start, count) runs."""
        bm = bm if bm is not None else self.bitmap()
        runs, start = [], None
        for b in range(self.total_blocks):
            used = bm[b >> 3] & (0x80 >> (b & 7))
            if used and start is None:
                start = b
            elif not used and start is not None:
                runs.append((start, b - start))
                start = None
        if start is not None:
            runs.append((start, self.total_blocks - start))
        return runs

    def allocate(self, count):
        """Mark `count` contiguous free blocks used; their first block."""
        bm = self.bitmap()
        run = 0
        for b in range(self.total_blocks):
            if bm[b >> 3] & (0x80 >> (b & 7)):
                run = 0
                continue
            run += 1
            if run == count:
                first = b - count + 1
                for k in range(first, b + 1):
                    bm[k >> 3] |= 0x80 >> (k & 7)
                self.write_bitmap(bm)
                self.free_blocks -= count
                self.write_header()
                return first
        raise OSError(f"{self.path}: no {count} contiguous free blocks")

    def write_header(self):
        struct.pack_into(">II", self.vh, 44, self.total_blocks, self.free_blocks)
        self.f.seek(1024)
        self.f.write(self.vh)
        self.f.seek(self.total_blocks * self.block_size - 1024)
        self.f.write(self.vh)
        self.f.flush()

    def grow(self, total_blocks, make_sparse=None):
        """Extend the volume to total_blocks; the new blocks are free, the last holds the alternate header."""
        old = self.total_blocks
        if total_blocks <= old:
            return
        if total_blocks > self.alloc.logical * 8:
            raise ValueError(f"{self.path}: the allocation file maps {self.alloc.logical * 8} blocks at most")
        self.f.flush()
        size = total_blocks * self.block_size
        if make_sparse:
            make_sparse(self.path, size)
        else:
            self.f.truncate(size)
        # the old alternate header's block (the last one, marked used) becomes free; the new last is used
        self.f.seek(old * self.block_size - 1024)
        self.f.write(bytes(1024))
        bm = self.bitmap()
        bm[(old - 1) >> 3] &= ~(0x80 >> ((old - 1) & 7)) & 0xff
        last = total_blocks - 1
        bm[last >> 3] |= 0x80 >> (last & 7)
        self.write_bitmap(bm)
        self.total_blocks = total_blocks
        self.free_blocks += total_blocks - old
        self.write_header()

    # ---- edits ----------------------------------------------------------------------------------------
    def write_catalog_bytes(self, at, data):
        self.catalog[at:at + len(data)] = data
        self.f.seek(self.fork_offset(self.catalog_fork, at))
        self.f.write(data)

    def truncate(self, path):
        """Empty the data fork of the file at `path`, its blocks back to the free pool: the donor of blocks
        on a full volume (the IPSW's root has none free). Only for forks whose extents are all in the
        record (none in the overflow file); the number of blocks freed."""
        rec = self.resolve(self.lookup(path))
        if not isinstance(rec, FileRecord):
            raise IsADirectoryError(path)
        if rec.compressed or sum(c for _, c in rec.data.extents) != rec.data.total_blocks:
            raise ValueError(f"{path}: compressed or with extents in the overflow file")
        bm = self.bitmap()
        for start, count in rec.data.extents:
            for b in range(start, start + count):
                bm[b >> 3] &= ~(0x80 >> (b & 7)) & 0xff
        self.write_bitmap(bm)
        self.free_blocks += rec.data.total_blocks
        self.write_header()
        r = bytearray(self.catalog[rec.at + rec.data_off:rec.at + rec.data_off + 248])
        r[88:168] = Fork.pack(0, [])
        self.write_catalog_bytes(rec.at + rec.data_off, bytes(r))
        self.f.flush()
        return rec.data.total_blocks

    def _folder_record(self, folder_id):
        """(offset in the catalog, data offset, type, length) of the folder record whose ID is folder_id."""
        for at, do, rtype, rlen in self.index().values():
            if rtype == REC_FOLDER and u32(self.catalog, at + do + 8) == folder_id:
                return at, do, rtype, rlen
        raise FileNotFoundError(f"{self.path}: no folder record for ID {folder_id}")

    def create_file(self, path, data=b"", mode=0o644, uid=0, gid=0):
        """A new file at `path` (its folder must exist) with `data` as an uncompressed data fork; its
        catalog record goes into the leaf node its key sorts into, which must have room for it (the one
        restructuring this module does not do is a node split). HFSX (binary-compared keys) only. No file
        thread record is made and none is flagged: they are optional in HFS+. Returns the new file ID."""
        if self.vh[0:2] != b"HX" or self.catalog[14 + 37] != 0xBC:
            raise ValueError(f"{self.path}: create_file needs HFSX with binary key compare")
        parts = [p for p in path.split("/") if p]
        parent = self.lookup("/" + "/".join(parts[:-1]))
        if not isinstance(parent, int):
            raise NotADirectoryError(path)
        name = parts[-1]
        if (parent, name) in self.index():
            raise FileExistsError(path)
        enc = name.encode("utf-16-be")
        key = struct.pack(">HIH", 6 + len(enc), parent, len(enc) // 2) + enc
        sort_key = (parent, [u16(enc, i) for i in range(0, len(enc), 2)])

        # the folder's record: its dates for the file's, and its valence to count the file
        pat, pdo, _, plen = self._folder_record(parent)
        prec = bytes(self.catalog[pat:pat + plen])
        fid = u32(self.vh, 64)
        n = -(-len(data) // self.block_size)
        extents = []
        if n:
            first = self.allocate(n)
            self.f.seek(first * self.block_size)
            self.f.write(bytes(data).ljust(n * self.block_size, b"\0"))
            extents = [(first, n)]
        when = u32(prec, pdo + 16)
        rec = bytearray(248)
        struct.pack_into(">HHII", rec, 0, REC_FILE, 0, 0, fid)
        struct.pack_into(">IIIII", rec, 12, when, when, when, when, 0)
        struct.pack_into(">IIBBHI", rec, 32, uid, gid, 0, 0, 0o100000 | mode, 0)
        rec[88:168] = Fork.pack(len(data), extents)
        rec[168:248] = Fork.pack(0, [])
        record = key + bytes(rec)

        # the leaf and the place in it: after the last record whose key sorts before the new one
        node_size = self.cat_node
        target = None
        for idx, at, r in self.leaf_records(self.catalog):
            rk = (u32(r, 2), [u16(r, 8 + i) for i in range(0, 2 * u16(r, 6), 2)])
            if rk < sort_key:
                target = (idx, (at - idx * node_size))
            else:
                break
        if target is None:
            raise ValueError(f"{path}: would be the catalog's first key")
        idx, after = target
        base = idx * node_size
        node = bytearray(self.catalog[base:base + node_size])
        nrec = u16(node, 10)
        offs = [u16(node, node_size - 2 * (i + 1)) for i in range(nrec + 1)]
        pos = offs.index(after) + 1                      # the new record's index in the node
        free_end = node_size - 2 * (nrec + 2)
        if offs[nrec] + len(record) > free_end:
            raise OSError(f"{path}: catalog node {idx} has {free_end - offs[nrec]} bytes free, the record needs "
                          f"{len(record)} (a node split is not implemented)")
        at = offs[pos]
        node[at + len(record):offs[nrec] + len(record)] = node[at:offs[nrec]]
        node[at:at + len(record)] = record
        offs = offs[:pos] + [at] + [o + len(record) for o in offs[pos:]]
        for i, o in enumerate(offs):
            struct.pack_into(">H", node, node_size - 2 * (i + 1), o)
        struct.pack_into(">H", node, 10, nrec + 1)
        self.write_catalog_bytes(base, bytes(node))

        # the B-tree header's leaf record count, the folder's valence, the volume's file count and next ID
        hdr = bytearray(self.catalog[14:14 + 10])
        struct.pack_into(">I", hdr, 6, u32(hdr, 6) + 1)
        self.write_catalog_bytes(14, bytes(hdr))
        self._index = None              # the insert moved records after it, the folder's own perhaps
        pat, pdo, _, plen = self._folder_record(parent)
        prec = bytearray(self.catalog[pat:pat + plen])
        struct.pack_into(">I", prec, pdo + 4, u32(prec, pdo + 4) + 1)
        self.write_catalog_bytes(pat, bytes(prec))
        struct.pack_into(">I", self.vh, 32, u32(self.vh, 32) + 1)
        struct.pack_into(">I", self.vh, 64, fid + 1)
        self.write_header()
        self._index = None
        return fid

    def set_file_data(self, path, data, mode=None):
        """Give the file at `path` (it must exist) `data` as an uncompressed data fork: in the fork's own
        blocks when it fits there (a full volume, as the IPSW's root is), else in fresh ones."""
        rec = self.resolve(self.lookup(path))
        if not isinstance(rec, FileRecord):
            raise IsADirectoryError(path)
        n = max(1, -(-len(data) // self.block_size))
        if not rec.compressed and rec.data.extents and sum(c for _, c in rec.data.extents) >= n:
            extents, off = [], 0
            for start, count in rec.data.extents:
                take = min(count, n - sum(c for _, c in extents))
                if take <= 0:
                    break
                self.f.seek(start * self.block_size)
                self.f.write(bytes(data[off:off + take * self.block_size]).ljust(take * self.block_size, b"\0"))
                extents.append((start, take))
                off += take * self.block_size
            first = extents[0][0]
        else:
            first = self.allocate(n)
            self.f.seek(first * self.block_size)
            self.f.write(bytes(data).ljust(n * self.block_size, b"\0"))
            extents = [(first, n)]
        r = bytearray(self.catalog[rec.at + rec.data_off:rec.at + rec.data_off + 248])
        r[41] &= ~UF_COMPRESSED & 0xff
        if mode is not None:
            struct.pack_into(">H", r, 42, (u16(r, 42) & 0o170000) | mode)
        r[88:168] = Fork.pack(len(data), extents)
        self.write_catalog_bytes(rec.at + rec.data_off, bytes(r))
        self.f.flush()
        return first, n
