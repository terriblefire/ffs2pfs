"""Crash-safe OFS/FFS -> PFS3 conversion: anchors, journal, steps, resume.

On-disk state, found again after any crash (including a block torn half way):

  partition block 0 / 1   anchor A / B  (magic 'F2PA', CRC: plan and journal locations)
  journal A / B           one block each (magic 'F2PJ', CRC): seq, phase, next step
  plan A / B              two copies of the plan (plan.py, CRC), incl. the original
                          PART block and every RDB write
  RDB PART DosType        'F2P\\0' from the lock to the commit

Write order (any write may be the last, or be torn):

  START     plan A, plan B, journal A, journal B, anchor B (block 1)
            -> volume intact: block 1 is not used by OFS/FFS on a hard disk
  LOCK      anchor A over the boot block (FFS: "Not a DOS disk" from here),
            RDB DosType := 'F2P\\0' (nothing mounts the partition)
  RELOCATE  FFS: copy each data block out of the PFS3 reserved area
            OFS: copy every file, packed into 512-byte blocks, into free blocks
            (idempotent: nothing writes a source until the phase is over)
  BUILD     write the PFS3 metadata into the reserved area, from the plan (idempotent)
  RDBFS     put pfs3aio into the RDB (rdbfs.py, idempotent)
  COMMIT    block 1 := 'done' marker      (anchor B gone, anchor A still valid)
            RDB DosType := PFS\\3           (anchor A still valid: a torn PART block
                                             can be rebuilt from the plan)
            block 0 := PFS3 boot block    (anchor A gone; the marker says block 0 is ours)
            block 1 := zeros              (as format leaves it; pfs3aio never reads block 1)

Each phase ends with a read-back of everything it wrote; a block that does
not read back as written is written again before the journal moves on.

Finding the state: the RDB first. No valid RDSK: restore it from the rescue
copy (rdbfs.py). A PART block that fails its checksum is found again by its
anchor at the start of a cylinder and rewritten from the plan's copy.
"""
import struct, zlib
from . import rdb as rdbmod
from . import plan as planmod
from . import pfs3build
from . import rdbfs
from .blockdev import Part, BS
from .ffs import FFS, ofs_file_data

ANCHOR = b'F2PA'
JOURNAL = b'F2PJ'
DOSTYPE_CONVERTING = 0x46325000           # 'F2P\0'
DOSTYPE_PFS3 = rdbfs.DOSTYPE_PFS3         # 'PFS\3'
CHECKPOINT = 64
PH_RELOCATE, PH_BUILD, PH_RDBFS, PH_COMMIT = 1, 2, 3, 4
PFS3AIO_VERSION = (19 << 16) | 2


class StateError(Exception):
    pass


def _crcblock(fmt, *fields, magic):
    b = bytearray(BS)
    b[:4] = magic
    struct.pack_into('>' + fmt, b, 4, *fields)
    struct.pack_into('>I', b, BS - 4, zlib.crc32(bytes(b[:BS - 4])))
    return bytes(b)


def _checkcrc(b, magic):
    return b[:4] == magic and struct.unpack_from('>I', b, BS - 4)[0] == zlib.crc32(b[:BS - 4])


ANCHOR_FIELDS = ('version', 'part_start', 'part_blocks', 'planA', 'planB', 'plan_blocks',
                 'jA', 'jB', 'partblock', 'orig_dostype')
JOURNAL_FIELDS = ('seq', 'phase', 'next', 'plan_crc')


def make_anchor(a):
    return _crcblock('I' * len(ANCHOR_FIELDS), *[a[k] for k in ANCHOR_FIELDS], magic=ANCHOR)


def read_anchor(b):
    if not _checkcrc(b, ANCHOR):
        return None
    return dict(zip(ANCHOR_FIELDS, struct.unpack_from('>' + 'I' * len(ANCHOR_FIELDS), b, 4)))


def make_journal(seq, phase, nxt, plan_crc):
    return _crcblock('IIII', seq, phase, nxt, plan_crc, magic=JOURNAL)


def read_journal(b):
    if not _checkcrc(b, JOURNAL):
        return None
    return dict(zip(JOURNAL_FIELDS, struct.unpack_from('>IIII', b, 4)))


def done_marker():
    b = bytearray(BS)
    b[:4] = b'F2PD'
    struct.pack_into('>I', b, BS - 4, zlib.crc32(bytes(b[:BS - 4])))
    return bytes(b)


DONE = done_marker()


def is_dos_boot(b):
    return b[:3] == b'DOS' and b[3] <= 5


def discover(dev):
    """Partitions with their anchors. No valid RDSK: restore it from the rescue
    copy. A PART block that fails its checksum is found again by its anchor at
    the start of a cylinder."""
    if rdbmod.find_rdsk(dev)[1] is None:
        r = rdbfs.find_rescue(dev)
        if r is None:
            raise StateError("no valid RDSK and no rescue copy")
        dev.write(r[0], r[1])
    out = []
    parts = rdbmod.partitions(dev)
    for p in parts:
        if p['invalid']:
            continue
        b0, b1 = dev.read(p['start']), dev.read(p['start'] + 1)
        out.append(dict(p, aA=read_anchor(b0), aB=read_anchor(b1), b0=b0, b1=b1))
    if any(p['invalid'] for p in parts):
        g = rdbmod.geometry(dev)
        known = {p['start'] for p in out}
        for c in range(g['locyl'], g['hicyl'] + 1):
            blk = c * g['cylblocks']
            if blk in known:
                continue
            b0, b1 = dev.read(blk), dev.read(blk + 1)
            aA, aB = read_anchor(b0), read_anchor(b1)
            a = aA or aB
            if a and a['part_start'] == blk:
                out.append(dict(block=a['partblock'], name=None, start=blk, blocks=a['part_blocks'],
                                dostype=None, invalid=True, aA=aA, aB=aB, b0=b0, b1=b1))
    return out


class Converter:
    def __init__(self, dev, pinfo, binary=None, log=None):
        self.dev, self.p, self.binary = dev, pinfo, binary
        self.part = Part(dev, pinfo['start'], pinfo['blocks'])
        self.log = log or (lambda s: None)

    # ------------------------------------------------------------ helpers
    def _plan(self, a):
        for start in (a['planA'], a['planB']):
            body = planmod.parse(self.part.read(start, a['plan_blocks']))
            if body:
                return body
        raise StateError("both plan copies are bad")

    def _journal(self, a):
        js = [j for j in (read_journal(self.part.read(a['jA'])), read_journal(self.part.read(a['jB']))) if j]
        if not js:
            raise StateError("both journal copies are bad")
        return max(js, key=lambda j: j['seq'])

    def _set_part(self, dostype):
        """Rewrite the PART block from the plan's copy with this DosType (also repairs a torn one)."""
        b = bytearray(bytes.fromhex(self.body['part_orig']))
        struct.pack_into('>I', b, (32 + 16) * 4, dostype)
        self.dev.write(self.body['part_block'], rdbmod.rdb_checksum(b))
        self.p['dostype'] = dostype
        self.p['invalid'] = False

    def _checkpoint(self, phase, nxt):
        self.seq += 1
        slot = self.a['jA'] if self.seq % 2 else self.a['jB']
        self.part.write(slot, make_journal(self.seq, phase, nxt, self.plan_crc))

    def _readback(self, writes, dev=False, tries=3):
        """writes: [(block, bytes)], partition-relative (dev=False) or absolute.
        Rewrite whatever does not read back; give up after a few tries."""
        rd = self.dev.read if dev else self.part.read
        wr = self.dev.write if dev else self.part.write
        for _ in range(tries):
            bad = [(k, d) for k, d in writes if rd(k, len(d) // BS) != d]
            if not bad:
                return
            self.log("read-back: %d blocks rewritten" % len(bad))
            for k, d in bad:
                wr(k, d)
            writes = bad
        raise StateError("%d blocks do not read back as written" % len(writes))

    # ------------------------------------------------------------ the run
    def state(self):
        """('pfs3' | 'bootfix' | 'locked' | 'ffs', anchor), from what is on disk only."""
        p = self.p
        a = p['aA'] or p['aB']
        if p['invalid']:
            if not a:
                raise StateError("PART block bad and no anchor")
            return 'locked', a
        dt = p['dostype']
        if a and (dt in (DOSTYPE_CONVERTING, DOSTYPE_PFS3) or p['aA'] or not is_dos_boot(p['b0'])):
            return 'locked', a          # anchor A there, or A torn over a boot block DOS can't use
        if dt == DOSTYPE_PFS3:
            if p['b1'] == DONE:
                return 'bootfix', None   # block 0 (maybe torn) and block 1 still to write
            return 'pfs3', None
        if dt == DOSTYPE_CONVERTING:
            raise StateError("partition marked converting but no anchor found")
        return 'ffs', None               # never started, or the lock not reached: volume intact

    def run(self):
        st, a = self.state()
        self.log("state: %s" % st)
        if st == 'pfs3':
            return 'done'
        if st == 'bootfix':
            self.part.write(0, pfs3build.boot0())
            self.part.write(1, pfs3build.boot1())
            return 'done'
        if st == 'ffs':
            a = self.start()
        self.a = a
        self.body = self._plan(a)
        j = self._journal(a)
        self.seq, self.plan_crc = j['seq'], j['plan_crc']
        phase, nxt = j['phase'], j['next']
        if self.p['invalid']:                               # torn PART block: rebuild it
            self._set_part(DOSTYPE_PFS3 if phase == PH_COMMIT else DOSTYPE_CONVERTING)
        if phase < PH_COMMIT and self.p['dostype'] != DOSTYPE_CONVERTING:
            self.part.write(0, make_anchor(a))              # anchor A (again, if it was torn)
            self._set_part(DOSTYPE_CONVERTING)              # LOCK, part 2
        if phase == PH_RELOCATE:
            (self._copy if self.body['ofs'] else self._relocate)(nxt)
            phase, nxt = PH_BUILD, 0
        if phase == PH_BUILD:
            self._build(nxt)
            phase, nxt = PH_RDBFS, 0
        if phase == PH_RDBFS:
            self._rdbfs(nxt)
        self._commit()
        return 'done'

    def start(self):
        if self.binary is None:
            raise StateError("the pfs3aio binary is needed to start a conversion")
        ffs = FFS(self.part).read()
        body, targets = planmod.build(self.part, ffs)
        body['part_block'] = self.p['block']
        body['part_orig'] = self.dev.read(self.p['block']).hex()
        body['rdbfs'] = rdbfs.plan(self.dev, self.binary, PFS3AIO_VERSION,
                                   [x for x in rdbmod.partitions(self.dev) if not x['invalid']])
        data = planmod.serialise(body)
        pb = len(data) // BS
        lay = body['layout']
        exclude = set(targets) | set(range(lay['first'], lay['end']))
        jA, jB, pA, pB = planmod.alloc_runs(ffs.free, exclude, [1, 1, pb, pb])
        crc = zlib.crc32(data)
        self.log("start: %s, %d moves, plan %d blocks at %d/%d, journal %d/%d, RDB: %s" % (
            'OFS copy' if body['ofs'] else 'FFS', len(body['moves']), pb, pA, pB, jA, jB,
            'pfs3aio %s already there' % body['rdbfs']['existing_version'] if 'existing' in body['rdbfs']
            else '%d writes for pfs3aio' % len(body['rdbfs']['writes'])))
        self.part.write(pA, data)
        self.part.write(pB, data)
        self.part.write(jA, make_journal(1, PH_RELOCATE, 0, crc))
        self.part.write(jB, make_journal(2, PH_RELOCATE, 0, crc))
        self._readback([(pA, data), (pB, data)])
        a = dict(version=1, part_start=self.p['start'], part_blocks=self.part.n, planA=pA, planB=pB,
                 plan_blocks=pb, jA=jA, jB=jB, partblock=self.p['block'], orig_dostype=self.p['dostype'])
        self.part.write(1, make_anchor(a))               # anchor B: volume still intact
        self.part.write(0, make_anchor(a))               # anchor A: LOCK (OFS/FFS no longer mounts)
        return a

    def _relocate(self, nxt):
        moves = self.body['moves']
        for i in range(nxt, len(moves)):
            src, dst = moves[i]
            self.part.write(dst, self.part.read(src))
            if (i + 1) % CHECKPOINT == 0:
                self._checkpoint(PH_RELOCATE, i + 1)
        self._readback([(dst, self.part.read(src)) for src, dst in moves])
        self._softlinks()
        self._checkpoint(PH_BUILD, 0)                     # barrier: sources may now be overwritten

    def _softlinks(self):
        """Soft link paths into their new data blocks (free blocks: idempotent)."""
        blocks = pfs3build.softlink_blocks(self.body)
        for k, d in blocks:
            self.part.write(k, d)
        self._readback(blocks)

    def _copy_files(self):
        """(file entry, [target blocks]) for every OFS file, in plan order."""
        for e in self.body['entries']:
            if e['kind'] == 'file':
                yield e, [k for a, c in e['pfs_extents'] for k in range(a, a + c)]

    def _copy(self, nxt):
        i = 0
        for e, blocks in self._copy_files():
            n = len(blocks)
            if i + n > nxt:
                data = ofs_file_data(self.part, e)
                data += bytes(-len(data) % BS)
                for j in range(max(0, nxt - i), n):
                    self.part.write(blocks[j], data[j * BS:(j + 1) * BS])
                    if (i + j + 1) % CHECKPOINT == 0:
                        self._checkpoint(PH_RELOCATE, i + j + 1)
            i += n
        for e, blocks in self._copy_files():              # read-back, file by file
            if blocks:
                data = ofs_file_data(self.part, e)
                data += bytes(-len(data) % BS)
                self._readback([(k, data[j * BS:(j + 1) * BS]) for j, k in enumerate(blocks)])
        self._softlinks()
        self._checkpoint(PH_BUILD, 0)                     # barrier: OFS blocks may now be overwritten

    def _build(self, nxt):
        blocks = pfs3build.reserved_blocks(self.body)
        for i in range(nxt, len(blocks)):
            k, data = blocks[i]
            self.part.write(k, data)
            if (i + 1) % CHECKPOINT == 0:
                self._checkpoint(PH_BUILD, i + 1)
        self._readback(blocks)
        self._checkpoint(PH_RDBFS, 0)

    def _rdbfs(self, nxt):
        r = self.body['rdbfs']
        writes = [(k, bytes.fromhex(d)) for k, d in r.get('writes', [])]
        for i in range(nxt, len(writes)):
            self.dev.write(*writes[i])
            if (i + 1) % CHECKPOINT == 0:
                self._checkpoint(PH_RDBFS, i + 1)
        last = {}
        for k, d in writes:                               # what each block holds at the end
            last[k] = d
        self._readback(sorted(last.items()), dev=True)
        self._checkpoint(PH_COMMIT, 0)

    def _commit(self):
        self.part.write(1, DONE)                           # anchor B gone, anchor A still valid
        if self.p['dostype'] != DOSTYPE_PFS3:
            self._set_part(DOSTYPE_PFS3)                   # anchor A still valid: a torn PART can be rebuilt
        self.part.write(0, pfs3build.boot0())              # anchor A gone, the marker remains
        self.part.write(1, pfs3build.boot1())              # as format leaves it


def in_progress(p):
    return (p['invalid'] or p['aA'] or p['aB'] or p['dostype'] == DOSTYPE_CONVERTING
            or (p['dostype'] == DOSTYPE_PFS3 and p['b1'] == DONE))


def run(dev, name=None, binary=None, log=None):
    """Resume a conversion in progress, or start one on the named OFS/FFS partition."""
    parts = discover(dev)
    busy = [p for p in parts if in_progress(p)]
    target = busy[0] if busy else next(p for p in parts if not p['invalid'] and (name is None or p['name'] == name))
    return Converter(dev, target, binary, log).run()
