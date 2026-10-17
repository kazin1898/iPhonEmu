"""IMG3 containers (iPhone OS 2.x through iOS 9): tag walk and AES-CBC payload decryption.

An IMG3 is '3gmI' + u32 full size + u32 size-without-header + u32 signed length + u32 type, then tags
{u32 magic (reversed), u32 total, u32 length, body}. DATA holds the payload, encrypted with AES-CBC
under the published IV/key (KBAG holds the GID-wrapped one). The last partial block is encrypted too
(the cipher ran over the tag padding), so decrypt the length rounded up to 16 and truncate; 2.x images
instead left the tail in the clear (plain_tail=True).
"""
import struct

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

MAGIC = b"3gmI"


def tags(buf):
    """Yield (name, data, padded_body) for each tag."""
    if buf[:4] != MAGIC:
        raise ValueError("not an IMG3 container")
    off = 20
    while off + 12 <= len(buf):
        magic, total, length = struct.unpack_from("<4sII", buf, off)
        if total < 12:
            break
        body = buf[off + 12:off + total]
        yield magic[::-1].decode("latin-1"), body[:length], body
        off += total


def tag_dict(buf):
    return {name: (data, body) for name, data, body in tags(buf)}


def image_type(buf):
    t = tag_dict(buf).get("TYPE")
    return t[0][::-1].decode("latin-1") if t else None


def decrypt(buf, iv, key, plain_tail=False):
    """The DATA payload decrypted with iv/key (bytes). No KBAG means the payload is in the clear."""
    found = tag_dict(buf)
    if "DATA" not in found:
        raise ValueError("IMG3 has no DATA tag")
    data, body = found["DATA"]
    if "KBAG" not in found:
        return bytes(data)
    span = (len(data) // 16 * 16) if plain_tail else ((len(data) + 15) // 16 * 16)
    if span > len(body):
        raise ValueError("IMG3 DATA tag has no room for its final padded block")
    dec = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
    out = dec.update(bytes(body[:span])) + dec.finalize()
    return out + bytes(data[span:]) if plain_tail else out[:len(data)]
