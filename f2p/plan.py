"""The conversion plan: everything the PFS3 build needs, so the FFS metadata is
no longer needed once the plan is on disk.

  layout   PFS3 reserved area (pfs3aio format.c rules)
  entries  the FFS model (ffs.FFS.entries)
  moves    [(src, dst)]: FFS data blocks inside the reserved area -> free blocks
  crcs     per file CRC32 of its contents (for the final check)
  boot     the original partition blocks 0 and 1 (hex)

Serialised as zlib(JSON) with a header: magic 'F2PP', version, length, CRC32.
"""
import json, struct, zlib

BS = 512
MAGIC = b'F2PP'
VERSION = 1
MAXNUMRESERVED = 4096 + 255 * 1024 * 8
MAXSMALLDISK = 5 * 253 * 253 * 32
MAXDISKSIZE1K = 104 * 253 * 253 * 32
MAXDISKSIZE2K = 104 * 509 * 509 * 32


class PlanError(Exception):
    pass


def pfs3_layout(total):
    """pfs3aio MakeRootBlock / CalcNumReserved."""
    resblocksize = 1024
    if total > MAXSMALLDISK:
        resblocksize = 1024 if total <= MAXDISKSIZE1K else (2048 if total <= MAXDISKSIZE2K else 4096)
    taken, i = 32, 2048
    while i and i // 2 < total and i <= 0xffffffff:
        taken += taken * (10 if i >= 512 * 2048 else 14) // 16
        i <<= 1
    taken //= resblocksize // 1024
    taken = min(MAXNUMRESERVED, taken - 1)
    numreserved = (taken + 31) & ~0x1f
    end = (resblocksize // BS) * numreserved + 2
    return dict(resblocksize=resblocksize, numreserved=numreserved, first=2, end=end,
                supermode=total > MAXSMALLDISK)


def build(part, ffs):
    lay = pfs3_layout(part.n)
    lo, hi = lay['first'], lay['end']
    if hi >= part.n:
        raise PlanError("partition too small for PFS3")
    kinds = {e['idx']: e['kind'] for e in ffs.entries}
    for e in ffs.entries:
        if e['kind'] == 'softlink' and len(e['link'].encode('latin1')) >= BS:
            raise PlanError("soft link %s: path too long for PFS3" % e['name'])
        if e['kind'] == 'hardlink' and kinds[e['target']] not in ('file', 'dir'):
            raise PlanError("hard link %s points at a soft link: PFS3 has no such link" % e['name'])
    check_names(ffs.entries)
    free = sorted(k for k in ffs.free if not (lo <= k < hi))
    crcs = {e['idx']: ffs.file_crc(e) for e in ffs.entries if e['kind'] == 'file'}   # reads and checks all data
    if ffs.ofs:
        # OFS data blocks hold 488 bytes behind a header: every file is copied,
        # packed, into free blocks; the OFS blocks stay as they are until the commit
        need = sum((e['size'] + BS - 1) // BS for e in ffs.entries if e['kind'] == 'file')
        if len(free) < need:
            raise PlanError("OFS: %d free blocks outside the PFS3 reserved area, the packed data needs %d" % (len(free), need))
        i = 0
        targets = []
        for e in ffs.entries:
            if e['kind'] != 'file':
                continue
            n = (e['size'] + BS - 1) // BS
            runs = []
            for k in free[i:i + n]:
                if runs and runs[-1][0] + runs[-1][1] == k:
                    runs[-1][1] += 1
                else:
                    runs.append([k, 1])
            e['pfs_extents'] = runs
            targets += free[i:i + n]
            i += n
        moves = []
        free = free[i:]
    else:
        inside = sorted(k for k, t in ffs.kind.items() if t == 'data' and lo <= k < hi)
        if len(free) < len(inside):
            raise PlanError("not enough free space to move %d blocks out of the reserved area" % len(inside))
        # move targets: the top free blocks of the partition, away from where FFS keeps
        # its data, in ascending order so a run of blocks stays a run
        targets = sorted(free[::-1][:len(inside)])
        moves = list(zip(inside, targets))
        free = free[:len(free) - len(inside)]
    # soft links: PFS3 keeps the path in a data block of its own
    soft = [e for e in ffs.entries if e['kind'] == 'softlink']
    if len(free) < len(soft):
        raise PlanError("no free blocks for %d soft links" % len(soft))
    for e, k in zip(soft, free):
        e['pfs_extents'] = [[k, 1]]
        targets = list(targets) + [k]
    body = dict(version=VERSION, part_blocks=part.n, dostype=ffs.dostype, ofs=ffs.ofs, volname=ffs.volname,
                rootdate=ffs.rootdate, voldate=ffs.voldate, voldate_alt=ffs.voldate_alt,
                layout=lay, entries=ffs.entries, moves=moves,
                crcs=crcs, boot=[part.read(0).hex(), part.read(1).hex()])
    from .pfs3build import Builder, BuildError
    try:
        body['reserved_used'] = len(Builder(body).build())      # dry run: does it fit?
    except BuildError as e:
        raise PlanError(str(e))
    return body, set(targets)


def intl_upper(name):
    """pfs3aio's intltoupper: ASCII and Latin-1 letters (not the division sign)."""
    return ''.join(chr(ord(c) - 32) if ('a' <= c <= 'z' or (0xe0 <= ord(c) <= 0xfe and ord(c) != 0xf7)) else c
                   for c in name)


def check_names(entries):
    """PFS3 looks names up with international upper case; a non-international
    OFS/FFS directory can hold two names that are one name to PFS3."""
    seen = {}
    for e in entries:
        k = (e['parent'], intl_upper(e['name']))
        if k in seen:
            raise PlanError("%r and %r are the same name to PFS3" % (seen[k], e['name']))
        seen[k] = e['name']
        if len(e['name'].encode('latin1')) > 31:
            raise PlanError("name too long for PFS3: %r" % e['name'])


def serialise(body):
    raw = zlib.compress(json.dumps(body, separators=(',', ':')).encode(), 9)
    hdr = MAGIC + struct.pack('>III', VERSION, len(raw), zlib.crc32(raw))
    data = hdr + raw
    data += bytes(-len(data) % BS)
    return data


def parse(data):
    if data[:4] != MAGIC:
        return None
    ver, n, crc = struct.unpack_from('>III', data, 4)
    raw = data[16:16 + n]
    if ver != VERSION or len(raw) != n or zlib.crc32(raw) != crc:
        return None
    body = json.loads(zlib.decompress(raw))
    body['crcs'] = {int(k): v for k, v in body['crcs'].items()}
    return body


def alloc_runs(free, exclude, sizes):
    """Contiguous runs for the tool area (journal, plan copies) from free blocks not in exclude.
    Returns the first block of each run, or raises."""
    avail = sorted(set(free) - set(exclude))
    out, used = [], set()
    for size in sizes:
        run_start, run_len, prev, found = None, 0, None, None
        for k in avail:
            if k in used:
                prev = None; run_len = 0; continue
            if prev is not None and k == prev + 1:
                run_len += 1
            else:
                run_start, run_len = k, 1
            prev = k
            if run_len == size:
                found = run_start; break
        if found is None:
            raise PlanError("no free run of %d blocks for the conversion journal" % size)
        out.append(found)
        used.update(range(found, found + size))
    return out
