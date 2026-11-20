"""RDB (RDSK/PART) reading, the DosType switch, and a small RDB writer for test images."""
import struct

BS = 512


def u32(b, i):
    return struct.unpack_from('>I', b, i * 4)[0]


def s32(b, i):
    return struct.unpack_from('>i', b, i * 4)[0]


def rdb_checksum(b):
    """RDB blocks: rdb_ChkSum (longword 2) makes the sum of the first SummedLongs longwords zero."""
    b = bytearray(b)
    n = u32(b, 1)
    struct.pack_into('>I', b, 8, 0)
    s = sum(struct.unpack_from('>%dI' % n, b, 0)) & 0xffffffff
    struct.pack_into('>I', b, 8, (-s) & 0xffffffff)
    return bytes(b)


def rdb_valid(b):
    n = u32(b, 1)
    return 0 < n <= 128 and sum(struct.unpack_from('>%dI' % n, b, 0)) & 0xffffffff == 0


def find_rdsk(dev):
    """First RDSK in blocks 0..15 with a valid checksum, as the boot ROM finds it."""
    for blk in range(16):
        b = dev.read(blk)
        if b[:4] == b'RDSK' and rdb_valid(b):
            return blk, b
    return None, None


def partitions(dev):
    blk, r = find_rdsk(dev)
    if r is None:
        return []
    out = []
    nxt = u32(r, 7)
    seen = set()
    while nxt != 0xffffffff and nxt not in seen:
        seen.add(nxt)
        p = dev.read(nxt)
        if p[:4] != b'PART' or not rdb_valid(p):
            out.append(dict(block=nxt, invalid=True))
            break
        env = [u32(p, 32 + i) for i in range(20)]
        surf, bpt, lo, hi = env[3], env[5], env[9], env[10]
        out.append(dict(block=nxt, name=p[37:37 + p[36]].decode('latin1'),
                        start=lo * surf * bpt, blocks=(hi - lo + 1) * surf * bpt,
                        dostype=env[16], reserved=env[6], bootpri=s32(p, 32 + 15), invalid=False))
        nxt = u32(p, 4)
    return out


def geometry(dev):
    blk, r = find_rdsk(dev)
    return dict(cylblocks=u32(r, 36), locyl=u32(r, 34), hicyl=u32(r, 35)) if r else None


def set_dostype(dev, partblock, dostype):
    """Rewrite one PART block's de_DosType (one block write; idempotent)."""
    p = bytearray(dev.read(partblock))
    struct.pack_into('>I', p, (32 + 16) * 4, dostype)
    dev.write(partblock, rdb_checksum(p))


def make_rdb_disk(path, part_image, name='DH0', dostype=0x444F5301, rdb_blocks=256, fs_data=None, tight=False, spb=1):
    """Write a disk image: RDB area of rdb_blocks blocks, then the partition image.
    Geometry 1 head x 32 sectors; the partition starts at cylinder rdb_blocks/32.
    fs_data: put a DOS\\1 filesystem with this code into the RDB (FSHD + LSEGs)."""
    data = open(part_image, 'rb').read()
    assert len(data) % (32 * BS) == 0, "partition image must be whole cylinders of 32 blocks"
    cyls_part = len(data) // (32 * BS)
    lo = rdb_blocks // 32
    hi = lo + cyls_part - 1
    total_cyls = hi + 1
    rdsk = bytearray(BS)
    rdsk[:4] = b'RDSK'
    struct.pack_into('>I', rdsk, 4, 64)              # SummedLongs
    struct.pack_into('>I', rdsk, 12, 7)              # HostID
    struct.pack_into('>I', rdsk, 16, BS)             # BlockBytes
    struct.pack_into('>I', rdsk, 20, 0)              # Flags
    for i in (6, 8, 9):                              # BadBlockList, FileSysHeaderList, DriveInit
        struct.pack_into('>I', rdsk, i * 4, 0xffffffff)
    struct.pack_into('>I', rdsk, 7 * 4, 1)           # PartitionList -> block 1
    for i in range(10, 16):
        struct.pack_into('>I', rdsk, i * 4, 0xffffffff)
    struct.pack_into('>I', rdsk, 16 * 4, total_cyls)  # Cylinders
    struct.pack_into('>I', rdsk, 17 * 4, 32)          # Sectors
    struct.pack_into('>I', rdsk, 18 * 4, 1)           # Heads
    struct.pack_into('>I', rdsk, 32 * 4, 0)           # RDBBlocksLo
    struct.pack_into('>I', rdsk, 33 * 4, rdb_blocks - 1)
    struct.pack_into('>I', rdsk, 34 * 4, lo)          # LoCylinder
    struct.pack_into('>I', rdsk, 35 * 4, hi)
    struct.pack_into('>I', rdsk, 36 * 4, 32)          # CylBlocks
    part = bytearray(BS)
    part[:4] = b'PART'
    struct.pack_into('>I', part, 4, 64)
    struct.pack_into('>I', part, 12, 7)
    struct.pack_into('>I', part, 16, 0xffffffff)      # Next
    struct.pack_into('>I', part, 20, 1)               # Flags: bootable
    part[36] = len(name)
    part[37:37 + len(name)] = name.encode()
    env = [16, 128, 0, 1, spb, 32, 2, 0, 0, lo, hi, 30, 0, 0x1fe00, 0x7ffffffe, 0, dostype, 0, 0, 0]
    for i, v in enumerate(env):
        struct.pack_into('>I', part, (32 + i) * 4, v & 0xffffffff)
    blocks = [rdb_checksum(part)]
    if fs_data is not None:
        nseg = (len(fs_data) + 491) // 492
        struct.pack_into('>I', rdsk, 8 * 4, 2)                         # FileSysHeaderList
        fshd = bytearray(BS)
        fshd[:4] = b'FSHD'
        struct.pack_into('>IIII', fshd, 4, 64, 0, 7, 0xffffffff)
        struct.pack_into('>III', fshd, 32, 0x444F5301, (40 << 16) | 1, 0x180)
        struct.pack_into('>II', fshd, 72, 3, 0xffffffff)
        blocks.append(rdb_checksum(fshd))
        for i in range(nseg):
            seg = bytearray(BS)
            seg[:4] = b'LSEG'
            chunk = fs_data[i * 492:(i + 1) * 492]
            struct.pack_into('>IIII', seg, 4, 5 + len(chunk) // 4, 0, 7, 4 + i if i + 1 < nseg else 0xffffffff)
            seg[20:20 + len(chunk)] = chunk
            blocks.append(rdb_checksum(seg))
    struct.pack_into('>I', rdsk, 38 * 4, len(blocks))                 # HighRDSKBlock
    if tight:                                                          # RDB range = the blocks in use
        struct.pack_into('>I', rdsk, 33 * 4, len(blocks))
    assert len(blocks) + 1 <= rdb_blocks
    with open(path, 'wb') as f:
        f.write(rdb_checksum(rdsk))
        for b in blocks:
            f.write(b)
        f.write(bytes(BS * (rdb_blocks - 1 - len(blocks))))
        f.write(data)
    return dict(start=rdb_blocks, blocks=len(data) // BS)
