"""Stand-in for the PFS3 builder while the crash machinery is tested: deterministic
blocks for the reserved area and boot blocks, computed from the plan only."""
import struct, zlib
BS = 512


def boot0():
    b = bytearray(BS)
    b[:4] = b'PFS\x01'
    return bytes(b)


def boot1():
    """Second PFS3 boot block (unused by PFS3): a 'conversion done' marker, so a
    torn last write of block 0 is recognised as ours and finished."""
    b = bytearray(BS)
    b[:4] = b'F2PD'
    struct.pack_into('>I', b, BS - 4, zlib.crc32(bytes(b[:BS - 4])))
    return bytes(b)


def reserved_blocks(body):
    lay = body['layout']
    tag = zlib.crc32(body['volname'].encode()) & 0xffffffff
    out = []
    for k in range(lay['first'], lay['end']):
        b = bytearray(BS)
        b[:4] = b'P3SB'
        struct.pack_into('>II', b, 4, k, tag)
        out.append((k, bytes(b)))
    return out
