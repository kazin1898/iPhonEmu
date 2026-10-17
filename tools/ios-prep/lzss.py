"""Apple 'complzss' kernelcache expansion (Okumura LZSS, N=4096 F=18) with the Adler-32 check."""
import struct
import zlib

N, F, THRESHOLD, HEADER = 4096, 18, 2, 0x180


def decompress(src, cap):
    ring = bytearray(b" " * (N - F) + b"\0" * (F + F - 1))
    out = bytearray()
    r, flags, i, n = N - F, 0, 0, len(src)
    while True:
        flags >>= 1
        if not flags & 0x100:
            if i >= n:
                break
            flags = src[i] | 0xFF00
            i += 1
        if flags & 1:
            if i >= n or len(out) >= cap:
                break
            c = src[i]
            i += 1
            out.append(c)
            ring[r] = c
            r = (r + 1) & (N - 1)
        else:
            if i + 1 >= n:
                break
            off = src[i] | ((src[i + 1] & 0xF0) << 4)
            length = (src[i + 1] & 0x0F) + THRESHOLD
            i += 2
            for k in range(length + 1):
                if len(out) >= cap:
                    return bytes(out)
                c = ring[(off + k) & (N - 1)]
                out.append(c)
                ring[r] = c
                r = (r + 1) & (N - 1)
    return bytes(out)


def is_complzss(buf):
    return buf[:8] == b"complzss"


def complzss(buf):
    """The Mach-O inside a complzss blob; raises on a bad header or checksum."""
    if not is_complzss(buf) or len(buf) < HEADER:
        raise ValueError("not a complzss blob")
    adler, unc, cmp_ = struct.unpack(">III", buf[8:20])
    if HEADER + cmp_ > len(buf):
        raise ValueError("complzss: declared compressed size exceeds the blob")
    out = decompress(buf[HEADER:HEADER + cmp_], unc)
    if len(out) < unc:
        out += b"\0" * (unc - len(out))
    if zlib.adler32(out) & 0xFFFFFFFF != adler:
        raise ValueError("complzss: Adler-32 mismatch (wrong key?)")
    return out
