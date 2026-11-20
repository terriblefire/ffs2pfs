"""OFS/FFS (DOS\\0 to DOS\\5) reader: the whole volume as a model the plan can keep.

OFS data blocks (DOS\\0, \\2, \\4) carry a 24-byte header (type 8, header key,
sequence number, data size, next block, checksum) and 488 bytes of data; FFS
data blocks are 512 bytes of data. Dircache blocks (DOS\\4, \\5) are metadata
and are not needed by the conversion.

Entry fields: idx, parent (idx, -1 for the root's children), kind ('dir',
'file', 'softlink', 'hardlink'), name, protect, date (days, mins, ticks),
comment, size, extents [(first, count)] for files, link (target name or
header key), owner key (the FFS header block).
"""
import struct, zlib

BS = 512
HT = 72
T_HEADER, T_DATA, T_LIST = 2, 8, 16
OFS_DATA = 488
ST_ROOT, ST_USERDIR, ST_SOFTLINK, ST_LINKDIR, ST_FILE, ST_LINKFILE = 1, 2, 3, 4, -3, -4


def u32(b, i):
    return struct.unpack_from('>I', b, i * 4)[0]


def s32(b, i):
    return struct.unpack_from('>i', b, i * 4)[0]


def csum(b):
    return sum(struct.unpack('>128I', b)) & 0xffffffff


class FFSError(Exception):
    pass


class FFS:
    def __init__(self, part, reserved=2):
        self.p = part
        self.n = part.n
        self.reserved = reserved
        self.entries = []
        self.owner = {}          # block -> entry idx (-1 root, -2 bitmap, -3 boot)
        self.kind = {}           # block -> 'data', 'header', 'ext', 'bitmap', 'bmext', 'root', 'boot'
        self.problems = []
        self.free = set()

    def _claim(self, k, idx, kind):
        if k in self.owner:
            self.problems.append("block %d claimed twice" % k)
        self.owner[k] = idx
        self.kind[k] = kind

    def valid_header(self, k):
        if not (0 < k < self.n):
            return None
        b = self.p.read(k)
        if u32(b, 0) != T_HEADER or u32(b, 1) != k or csum(b) != 0:
            return None
        return b

    def find_root(self):
        mid = (self.n - 1 + self.reserved) // 2
        for k in (mid, u32(self.p.read(0), 2)):
            if 0 < k < self.n:
                b = self.p.read(k)
                if u32(b, 0) == T_HEADER and s32(b, 127) == ST_ROOT and csum(b) == 0:
                    return k
        raise FFSError("no valid FFS root")

    @staticmethod
    def bstr(b, off, maxlen):
        n = min(b[off], maxlen)
        return b[off + 1:off + 1 + n].decode('latin1')

    def read(self):
        boot = self.p.read(0)
        if boot[:3] != b'DOS' or boot[3] > 5:
            raise FFSError("not an OFS/FFS boot block (%r)" % boot[:4])
        self.dostype = struct.unpack('>I', boot[:4])[0]
        self.ofs = boot[3] in (0, 2, 4)
        self.bpd = OFS_DATA if self.ofs else BS          # data bytes per data block
        self.root = self.find_root()
        rb = self.p.read(self.root)
        self.volname = self.bstr(rb, 432, 30)
        self.rootdate = (u32(rb, 105), u32(rb, 106), u32(rb, 107))       # root dir altered
        self.voldate_alt = (u32(rb, 118), u32(rb, 119), u32(rb, 120))    # volume altered
        self.voldate = (u32(rb, 121), u32(rb, 122), u32(rb, 123))        # volume created
        self._claim(0, -3, 'boot')
        self._claim(1, -3, 'boot')
        self._claim(self.root, -1, 'root')
        self._dir(self.root, -1, 0)
        self._bitmap(rb)
        self._links()
        return self

    def _links(self):
        """Hard links: the target of each, and each object's FFS link chain must
        hold exactly the links that point at it."""
        by_key = {e['key']: e for e in self.entries}
        pointing = {}
        for e in self.entries:
            if e['kind'] == 'hardlink':
                t = by_key.get(e['link'])
                if t is None or t['kind'] not in ('file', 'dir', 'softlink'):
                    raise FFSError("hard link %s: target %d is not a file or directory" % (e['name'], e['link']))
                e['target'] = t['idx']
                pointing.setdefault(t['idx'], set()).add(e['idx'])
        for e in self.entries:
            if e['kind'] in ('file', 'dir', 'softlink'):
                chain, k, guard = set(), e['next_link'], 0
                while k:
                    l = by_key.get(k)
                    guard += 1
                    if l is None or l['kind'] != 'hardlink' or guard > 100000:
                        raise FFSError("%s: link chain broken at %d" % (e['name'], k))
                    chain.add(l['idx'])
                    k = l['next_link']
                if chain != pointing.get(e['idx'], set()):
                    raise FFSError("%s: link chain does not match its links" % e['name'])

    def _dir(self, k, parent, depth):
        if depth > 100:
            raise FFSError("directory nesting too deep")
        b = self.p.read(k)
        for i in range(HT):
            e, guard = u32(b, 6 + i), 0
            while e:
                guard += 1
                if guard > 100000:
                    raise FFSError("hash chain loop")
                h = self.valid_header(e)
                if h is None:
                    raise FFSError("bad header %d" % e)
                idx = len(self.entries)
                sec = s32(h, 127)
                ent = dict(idx=idx, parent=parent, key=e, name=self.bstr(h, 432, 30),
                           protect=u32(h, 80), date=(u32(h, 105), u32(h, 106), u32(h, 107)),
                           comment=self.bstr(h, 328, 79), size=0, extents=[], link=None,
                           next_link=u32(h, 118))                # first link (object) / next link (link)
                self.entries.append(ent)
                self._claim(e, idx, 'header')
                if sec == ST_USERDIR:
                    ent['kind'] = 'dir'
                    self._dir(e, idx, depth + 1)
                elif sec == ST_FILE:
                    ent['kind'] = 'file'
                    self._file(e, h, ent)
                elif sec == ST_SOFTLINK:
                    ent['kind'] = 'softlink'
                    ent['link'] = h[24:24 + BS - 224].split(b'\0')[0].decode('latin1')
                elif sec in (ST_LINKDIR, ST_LINKFILE):
                    ent['kind'] = 'hardlink'
                    ent['link'] = u32(h, 117)            # real entry's header key
                else:
                    raise FFSError("unknown sec type %d at %d" % (sec, e))
                e = u32(h, 124)

    def _file(self, k, h, ent):
        blocks, hdr, guard = [], h, 0
        while True:
            cnt = u32(hdr, 2)
            if cnt > HT:
                raise FFSError("bad block count in %s" % ent['name'])
            blocks += [u32(hdr, 6 + HT - 1 - i) for i in range(cnt)]
            ext = u32(hdr, 126)
            if not ext:
                break
            guard += 1
            e = self.p.read(ext) if 0 < ext < self.n else None
            if guard > 200000 or e is None or u32(e, 0) != T_LIST or u32(e, 1) != ext or csum(e) != 0:
                raise FFSError("bad extension block %d in %s" % (ext, ent['name']))
            self._claim(ext, ent['idx'], 'ext')
            hdr = e
        ent['size'] = u32(h, 81)
        if len(blocks) != (ent['size'] + self.bpd - 1) // self.bpd:
            raise FFSError("%s: %d blocks for %d bytes" % (ent['name'], len(blocks), ent['size']))
        for d in blocks:
            if not (0 < d < self.n):
                raise FFSError("%s: data block %d out of range" % (ent['name'], d))
            self._claim(d, ent['idx'], 'data')
            if ent['extents'] and ent['extents'][-1][0] + ent['extents'][-1][1] == d:
                ent['extents'][-1][1] += 1
            else:
                ent['extents'].append([d, 1])

    def _bitmap(self, rb):
        if u32(rb, 78) != 0xffffffff:
            raise FFSError("bitmap not valid (volume needs validating)")
        bm = [u32(rb, 79 + i) for i in range(25) if u32(rb, 79 + i)]
        ext = u32(rb, 104)
        while ext:
            eb = self.p.read(ext)
            self._claim(ext, -2, 'bmext')
            bm += [u32(eb, i) for i in range(127) if u32(eb, i)]
            ext = u32(eb, 127)
        bit = 0
        for k in bm:
            b = self.p.read(k)
            self._claim(k, -2, 'bitmap')
            if csum(b) != 0:
                raise FFSError("bitmap block %d checksum bad" % k)
            for i in range(1, 128):
                v = u32(b, i)
                for j in range(32):
                    blk = self.reserved + bit
                    if blk < self.n and v & (1 << j):
                        self.free.add(blk)
                    bit += 1
        bad = [k for k in self.owner if k in self.free]
        if bad:
            raise FFSError("%d blocks in use but marked free (e.g. %d)" % (len(bad), bad[0]))

    def file_crc(self, ent, relocate=None):
        """CRC32 of a file's contents; relocate maps old data block -> new (FFS)."""
        if self.ofs:
            return zlib.crc32(ofs_file_data(self.p, ent))
        c, left = 0, ent['size']
        for first, cnt in ent['extents']:
            for d in range(first, first + cnt):
                src = relocate.get(d, d) if relocate else d
                b = self.p.read(src)
                c = zlib.crc32(b[:min(BS, left)], c)
                left -= BS
        return c


def ofs_file_data(part, ent):
    """A file's contents from its OFS data blocks, every block checked."""
    out, seq, left = bytearray(), 0, ent['size']
    for first, cnt in ent['extents']:
        for d in range(first, first + cnt):
            b = part.read(d)
            seq += 1
            n = u32(b, 3)
            if (u32(b, 0) != T_DATA or u32(b, 1) != ent['key'] or u32(b, 2) != seq or csum(b) != 0
                    or n != min(OFS_DATA, left)):
                raise FFSError("%s: OFS data block %d (#%d) is bad" % (ent['name'], d, seq))
            out += b[24:24 + n]
            left -= n
    if left:
        raise FFSError("%s: %d bytes missing" % (ent['name'], left))
    return bytes(out)
