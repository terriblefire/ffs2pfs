"""Put pfs3aio into the RDB, crash-safe.

The filesystem goes in as an FSHD block and a chain of LSEG blocks, in RDB
blocks no RDB structure uses. It is linked at the tail of the FSHD chain (the
last FSHD's Next), or from the RDSK when the chain is empty; the RDSK's
HighRDSKBlock is raised to cover the new blocks.

Every write is a pure function of the plan, so the whole sequence can be
redone after a crash. The RDSK is the one block that must never be left torn:
the boot ROM and this tool find everything through it. Before it is written,
a rescue copy of the new RDSK goes into a free RDB block (copy first, then a
marker block holding its CRC); a resume that finds no valid RDSK in blocks
0..15 scans the RDB area for the marker and restores the RDSK from the copy.
The marker is cleared once the RDSK is written.

  write order: LSEGs, FSHD, link (last FSHD), rescue copy, rescue marker,
               RDSK, marker := 0, copy := 0
"""
import struct, zlib
from .rdb import u32, rdb_checksum, rdb_valid

BS = 512
NOBLK = 0xffffffff
DOSTYPE_PFS3 = 0x50465303                  # 'PFS\3', pfs3aio's usual RDB DosType
RESCUE = b'F2PR'
LSEG_DATA = 123 * 4
RESCUE_SCAN = 65536                        # blocks searched for a rescue marker


class RDBError(Exception):
    pass


def find_rdsk(dev):
    """First RDSK in blocks 0..15 with a valid checksum, as the boot ROM finds it."""
    for blk in range(16):
        b = dev.read(blk)
        if b[:4] == b'RDSK' and rdb_valid(b):
            return blk, b
    return None, None


def walk(dev, rdsk):
    """All RDB blocks in use and the FSHD chain: (used set, [(fshd block, bytes, [lseg blocks])])."""
    used = set()

    def chain(nxt, magic):
        out = []
        while nxt != NOBLK:
            if nxt in used or nxt >= dev.nblocks:
                raise RDBError("RDB chain loop or bad pointer at %d" % nxt)
            b = dev.read(nxt)
            if b[:4] != magic or not rdb_valid(b):
                raise RDBError("RDB block %d: want a valid %r, found %r" % (nxt, magic, b[:4]))
            used.add(nxt)
            out.append((nxt, b))
            nxt = u32(b, 4)
        return out

    rblk, r = rdsk
    used.add(rblk)
    chain(u32(r, 7), b'PART')
    fs = [(blk, b, [x for x, _ in chain(u32(b, 18), b'LSEG')]) for blk, b in chain(u32(r, 8), b'FSHD')]
    chain(u32(r, 9), b'LSEG')                       # drive init code
    chain(u32(r, 6), b'BADB')
    return used, fs


def make_fshd(hostid, segstart, version):
    b = bytearray(BS)
    b[:4] = b'FSHD'
    struct.pack_into('>IIII', b, 4, 64, 0, hostid, NOBLK)
    struct.pack_into('>III', b, 32, DOSTYPE_PFS3, version, 0x180)      # patch SegList and GlobalVec
    struct.pack_into('>II', b, 72, segstart, NOBLK)                    # SegListBlocks, GlobalVec -1
    return rdb_checksum(b)


def make_lsegs(hostid, blocks, binary):
    out = []
    for i, blk in enumerate(blocks):
        b = bytearray(BS)
        b[:4] = b'LSEG'
        nxt = blocks[i + 1] if i + 1 < len(blocks) else NOBLK
        chunk = binary[i * LSEG_DATA:(i + 1) * LSEG_DATA]
        struct.pack_into('>IIII', b, 4, 5 + len(chunk) // 4, 0, hostid, nxt)   # SummedLongs: header + data
        b[20:20 + len(chunk)] = chunk
        out.append((blk, rdb_checksum(b)))
    return out


def plan(dev, binary, version, partitions):
    """What to write, or None when the RDB already has a PFS\\3 filesystem."""
    if len(binary) % 4:
        raise RDBError("filesystem binary length is not a multiple of 4")
    rblk, r = find_rdsk(dev)
    if r is None:
        raise RDBError("no valid RDSK block")
    used, fs = walk(dev, (rblk, r))
    for blk, b, segs in fs:
        if u32(b, 8) == DOSTYPE_PFS3:
            v = u32(b, 9)
            return dict(existing=blk, existing_version='%d.%d' % (v >> 16, v & 0xffff))
    lo, hi = u32(r, 32), u32(r, 33)
    # blocks no partition can ever hold: below LoCylinder and below every partition
    limit = min([u32(r, 34) * u32(r, 36)] + [p['start'] for p in partitions])
    nlseg = (len(binary) + LSEG_DATA - 1) // LSEG_DATA
    need = 1 + nlseg + 2                            # FSHD, LSEGs, rescue copy + marker
    # first unused blocks of the RDB range, then all-zero blocks past it (the range grows)
    free = [k for k in range(max(lo, 16), min(hi + 1, limit)) if k not in used]
    if len(free) < need:
        free += [k for k in range(max(hi + 1, 16), limit) if k not in used and not any(dev.read(k))][:need]
    if len(free) < need:
        raise RDBError("room for %d RDB blocks before the first partition, pfs3aio needs %d" % (len(free), need))
    take = free[:need]
    fshd_blk, lsegs, rescue = take[0], take[1:1 + nlseg], take[1 + nlseg:]
    hostid = u32(r, 3)
    writes = [(blk, data) for blk, data in make_lsegs(hostid, lsegs, binary)]
    writes.append((fshd_blk, make_fshd(hostid, lsegs[0], version)))
    newr = bytearray(r)
    if fs:
        last_blk, last, _ = fs[-1]
        lb = bytearray(last)
        struct.pack_into('>I', lb, 16, fshd_blk)
        writes.append((last_blk, rdb_checksum(lb)))
    else:
        struct.pack_into('>I', newr, 32, fshd_blk)  # FileSysHeaderList
    struct.pack_into('>I', newr, 38 * 4, max([u32(r, 38)] + list(used) + take[:-2]))   # HighRDSKBlock
    struct.pack_into('>I', newr, 33 * 4, max(hi, max(take)))                            # RDBBlocksHi
    newr = rdb_checksum(newr)
    marker = bytearray(BS)
    marker[:4] = RESCUE
    struct.pack_into('>III', marker, 4, rblk, rescue[0], zlib.crc32(newr))
    struct.pack_into('>I', marker, BS - 4, zlib.crc32(bytes(marker[:BS - 4])))
    writes += [(rescue[0], newr), (rescue[1], bytes(marker)), (rblk, newr),
               (rescue[1], bytes(BS)), (rescue[0], bytes(BS))]
    return dict(writes=[(blk, data.hex()) for blk, data in writes], fshd=fshd_blk, lsegs=lsegs,
                rescue=rescue, rdsk=rblk, binary_crc=zlib.crc32(binary), binary_len=len(binary))


def find_rescue(dev, limit=RESCUE_SCAN):
    """(rdsk block, new rdsk bytes) from a complete rescue copy, or None."""
    for k in range(16, min(limit, dev.nblocks)):
        b = dev.read(k)
        if b[:4] != RESCUE or struct.unpack_from('>I', b, BS - 4)[0] != zlib.crc32(b[:BS - 4]):
            continue
        rblk, copy_blk, crc = struct.unpack_from('>III', b, 4)
        c = dev.read(copy_blk)
        if zlib.crc32(c) == crc and rblk < 16:
            return rblk, c
    return None


def check(dev, binary):
    """Problems with the RDB after a conversion (empty list: fine)."""
    errs = []
    rblk, r = find_rdsk(dev)
    if r is None:
        return ["no valid RDSK"]
    try:
        used, fs = walk(dev, (rblk, r))
    except RDBError as e:
        return [str(e)]
    pfs = [(blk, b, segs) for blk, b, segs in fs if u32(b, 8) == DOSTYPE_PFS3]
    if not pfs:
        errs.append("no PFS\\3 filesystem in the RDB")
    elif binary is not None:
        blk, b, segs = pfs[0]
        data = b''.join(dev.read(s)[20:20 + (u32(dev.read(s), 1) - 5) * 4] for s in segs)
        if data != binary:
            errs.append("PFS\\3 filesystem in the RDB is not the pfs3aio binary")
    if u32(r, 38) < max(used):
        errs.append("HighRDSKBlock %d below used block %d" % (u32(r, 38), max(used)))
    if max(used) > u32(r, 33):
        errs.append("RDB block %d outside the RDB range (RDBBlocksHi %d)" % (max(used), u32(r, 33)))
    if find_rescue(dev, u32(r, 33) + 1):
        errs.append("rescue marker left in the RDB area")
    return errs
