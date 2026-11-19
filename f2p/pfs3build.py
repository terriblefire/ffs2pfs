"""Build the PFS3 metadata for a converted volume, from the plan only.

The layout follows pfs3aio 19.2's format (format.c, allocation.c, anodes.c,
directory.c): rootblock cluster with the reserved bitmap at sector 2, the
root block extension straight after it, then bitmap index and bitmap blocks,
anode index (and super index) blocks, anode blocks, two deldir blocks and
the directory blocks, all in the reserved area. File data stays where FFS put
it (after the moves out of the reserved area); each contiguous run of a file
is one anode.

reserved_blocks(body) -> [(sector, bytes)] for the reserved area (sector 2 on),
boot0() -> the PFS\\1 boot block. Pure functions of the plan: the same plan
always gives the same blocks, so BUILD can be redone after a crash.
"""
import struct

BS = 512
ID_PFS1 = 0x50465301
DBLKID, ABLKID, IBLKID, BMBLKID, BMIBLKID, DELDIRID, EXTENSIONID, SBLKID = (
    0x4442, 0x4142, 0x4942, 0x424D, 0x4D49, 0x4444, 0x4558, 0x5342)
MODE_HARDDISK, MODE_SPLITTED_ANODES, MODE_DIR_EXTENSION, MODE_DELDIR = 1, 2, 4, 8
MODE_SIZEFIELD, MODE_EXTENSION, MODE_DATESTAMP, MODE_SUPERINDEX = 16, 32, 64, 128
MODE_SUPERDELDIR, MODE_EXTROVING, MODE_LONGFN = 256, 512, 1024
OPTIONS = (MODE_HARDDISK | MODE_SPLITTED_ANODES | MODE_DIR_EXTENSION | MODE_DELDIR | MODE_SIZEFIELD |
           MODE_EXTENSION | MODE_DATESTAMP | MODE_SUPERDELDIR | MODE_EXTROVING | MODE_LONGFN)
PFS2VERSION = (19 << 16) | 2          # the handler release this layout matches
ST_FILE, ST_USERDIR, ST_SOFTLINK, ST_LINKDIR, ST_LINKFILE = -3, 2, 3, 4, -4
ANODE_ROOTDIR = 5
DELDIR_BLOCKS = 2
DELENTRY_PROT = 5
MAXSMALLBITMAPINDEX, MAXBITMAPINDEX, MAXSMALLINDEXNR, MAXSUPER = 4, 103, 98, 15
DATESTAMP = 1
FNSIZE = 32                           # 31 characters, as format sets it
CMSIZE = 80


class BuildError(Exception):
    pass


def boot0():
    b = bytearray(BS)
    struct.pack_into('>I', b, 0, ID_PFS1)
    return bytes(b)


def boot1():
    return bytes(BS)


class Builder:
    def __init__(self, body):
        self.body = body
        self.body_by_idx = {e['idx']: e for e in body['entries']}
        lay = body['layout']
        self.n = body['part_blocks']
        self.rbs = lay['resblocksize']
        self.rc = self.rbs // BS
        self.numreserved = lay['numreserved']
        self.first = 2
        self.end = lay['end']                       # first data sector
        self.supermode = self.n > 5 * 253 * 253 * 32
        self.anodesper = (self.rbs - 16) // 12
        self.indexper = (self.rbs - 12) // 4
        self.longsperbmb = self.rbs // 4 - 3
        self.blocks = {}                            # sector -> bytearray (one reserved block)
        self.next_res = 0                           # next reserved index to hand out

    # ------------------------------------------------------------ reserved area
    def res_sector(self, i):
        return self.first + i * self.rc

    def alloc(self):
        if self.next_res >= self.numreserved:
            raise BuildError("reserved area full (%d blocks)" % self.numreserved)
        s = self.res_sector(self.next_res)
        self.next_res += 1
        b = bytearray(self.rbs)
        self.blocks[s] = b
        return s, b

    @staticmethod
    def hdr(b, bid, seqnr):
        struct.pack_into('>HHII', b, 0, bid, 0, DATESTAMP, seqnr)

    # ------------------------------------------------------------ the build
    def build(self):
        body = self.body
        # rootblock cluster: rootblock + reserved bitmap (format.c MakeReservedBitmap)
        nb1k = 1 + len(range(125, self.numreserved // 32, 256))
        nroot = (1024 * nb1k + self.rbs - 1) // self.rbs
        self.rblkcluster = nroot * self.rc
        self.next_res = nroot
        rext_s, rext = self.alloc()

        # data bitmap: which data sectors are in use
        moves = {s: d for s, d in body['moves']}
        used = set()
        self.extents = {}
        for e in body['entries']:
            if e['kind'] == 'file':
                ext = []
                src = e['pfs_extents'] if 'pfs_extents' in e else e['extents']     # OFS: the packed copy
                for first, cnt in src:
                    for k in range(first, first + cnt):
                        k = moves.get(k, k)
                        if k < self.end or k >= self.n:
                            raise BuildError("%s: data block %d outside the data area" % (e['name'], k))
                        if k in used:
                            raise BuildError("data block %d used twice" % k)
                        used.add(k)
                        if ext and ext[-1][0] + ext[-1][1] == k:
                            ext[-1][1] += 1
                        else:
                            ext.append([k, 1])
                self.extents[e['idx']] = ext
            elif e['kind'] == 'softlink':
                k = e['pfs_extents'][0][0]
                if k < self.end or k >= self.n or k in used:
                    raise BuildError("soft link %s: bad data block %d" % (e['name'], k))
                used.add(k)
            elif e['kind'] not in ('dir', 'hardlink'):
                raise BuildError("%s: %s not supported" % (e['name'], e['kind']))
        nbits = self.n - self.end
        no_bmb = ((nbits + 31) // 32 + self.longsperbmb - 1) // self.longsperbmb
        maxbmi = (MAXBITMAPINDEX if self.supermode else MAXSMALLBITMAPINDEX) + 1
        if no_bmb > maxbmi * self.indexper:
            raise BuildError("partition too large for PFS3")
        nbmi = (no_bmb + self.indexper - 1) // self.indexper
        self.bmindex = []
        bmi_blocks = []
        for i in range(nbmi):
            s, b = self.alloc()
            self.hdr(b, BMIBLKID, i)
            self.bmindex.append(s)
            bmi_blocks.append(b)
        for seq in range(no_bmb):
            s, b = self.alloc()
            self.hdr(b, BMBLKID, seq)
            struct.pack_into('>I', bmi_blocks[seq // self.indexper], 12 + 4 * (seq % self.indexper), s)
            base = seq * self.longsperbmb * 32
            for li in range(self.longsperbmb):
                v = 0xffffffff
                b0 = base + li * 32
                for j in range(32):
                    bit = b0 + j
                    if bit < nbits and (self.end + bit) in used:
                        v &= ~(0x80000000 >> j)
                struct.pack_into('>I', b, 12 + 4 * li, v)
        self.blocksfree = nbits - len(used)

        # anodes: numbers first, blocks after (their count is known then)
        self.anodes = [(0, 0xffffffff, 0)] * ANODE_ROOTDIR      # 0..4 reserved, as format leaves them
        children = {}
        for e in body['entries']:
            children.setdefault(e['parent'], []).append(e)
        self.dir_anode = {-1: ANODE_ROOTDIR}
        self.anodes.append(None)                                # 5: root dir, filled with its blocks
        self.file_anode = {}
        for e in body['entries']:                               # head anodes
            nr = self.new_anode(None)
            (self.dir_anode if e['kind'] == 'dir' else self.file_anode)[e['idx']] = nr
        by_idx = {e['idx']: e for e in body['entries']}
        self.first_link = {}
        links = {}
        for e in body['entries']:
            if e['kind'] == 'softlink':                          # the path's data block
                self.set_anode(self.file_anode[e['idx']], (1, e['pfs_extents'][0][0], 0))
            elif e['kind'] == 'hardlink':
                links.setdefault(e['target'], []).append(e)
        for t, ls in links.items():
            # link node: (directory of the object, directory of the link, next link)
            tdir = self.dir_anode[by_idx[t]['parent']]
            for i, l in enumerate(ls):
                nxt = self.file_anode[ls[i + 1]['idx']] if i + 1 < len(ls) else 0
                self.set_anode(self.file_anode[l['idx']], (tdir, self.dir_anode[l['parent']], nxt))
            self.first_link[t] = self.file_anode[ls[0]['idx']]
        for e in body['entries']:                               # file chains
            if e['kind'] != 'file':
                continue
            ext = self.extents[e['idx']]
            head = self.file_anode[e['idx']]
            if not ext:
                self.set_anode(head, (0, 0xffffffff, 0))
                continue
            prev = head
            for i, (blk, cnt) in enumerate(ext):
                nr = head if i == 0 else self.new_anode(None)
                if i:
                    pc, pb, _ = self.get_anode(prev)
                    self.set_anode(prev, (pc, pb, nr))
                self.set_anode(nr, (cnt, blk, 0))
                prev = nr

        # deldir blocks
        self.deldir = []
        cdate = tuple(body['voldate'])
        for i in range(DELDIR_BLOCKS):
            s, b = self.alloc()
            self.hdr(b, DELDIRID, i)
            struct.pack_into('>I', b, 22, DELENTRY_PROT)
            struct.pack_into('>HHH', b, 26, *[x & 0xffff for x in cdate])
            self.deldir.append(s)

        # directory blocks (anodes for extra dir blocks come from new_anode)
        self._dirs(-1, children, 0)

        # anode blocks and the index above them
        nab = (len(self.anodes) + self.anodesper - 1) // self.anodesper
        nidx = (nab + self.indexper - 1) // self.indexper
        if self.supermode:
            nsup = (nidx + self.indexper - 1) // self.indexper
            if nsup > MAXSUPER + 1:
                raise BuildError("too many anodes")
        elif nidx > MAXSMALLINDEXNR + 1:
            raise BuildError("too many anodes")
        self.superindex, sup_blocks = [], []
        if self.supermode:
            for i in range(nsup):
                s, b = self.alloc()
                self.hdr(b, SBLKID, i)
                self.superindex.append(s)
                sup_blocks.append(b)
        self.index, idx_blocks = [], []
        for i in range(nidx):
            s, b = self.alloc()
            self.hdr(b, IBLKID, i)
            self.index.append(s)
            idx_blocks.append(b)
            if self.supermode:
                struct.pack_into('>I', sup_blocks[i // self.indexper], 12 + 4 * (i % self.indexper), s)
        for seq in range(nab):
            s, b = self.alloc()
            self.hdr(b, ABLKID, seq)
            struct.pack_into('>I', idx_blocks[seq // self.indexper], 12 + 4 * (seq % self.indexper), s)
            for k in range(self.anodesper):
                i = seq * self.anodesper + k
                if i < len(self.anodes):
                    struct.pack_into('>III', b, 16 + 12 * k, *self.anodes[i])
        self.curranseqnr = nab - 1

        # rext and the rootblock last: they hold the counts and roving pointers
        self._rext(rext)
        root = self._rootblock(rext_s)
        out = [(2 + i, root[i * BS:(i + 1) * BS]) for i in range(self.rblkcluster)]
        for s in sorted(self.blocks):
            data = bytes(self.blocks[s])
            out += [(s + i, data[i * BS:(i + 1) * BS]) for i in range(self.rc)]
        return out

    # ------------------------------------------------------------ anodes
    def new_anode(self, a):
        i = len(self.anodes)
        self.anodes.append(a)
        return self.anodenr(i)

    def anodenr(self, i):
        seq, off = divmod(i, self.anodesper)
        if seq > 0xffff:
            raise BuildError("too many anodes")
        return (seq << 16) | off

    def index_of(self, nr):
        return (nr >> 16) * self.anodesper + (nr & 0xffff)

    def get_anode(self, nr):
        return self.anodes[self.index_of(nr)]

    def set_anode(self, nr, a):
        self.anodes[self.index_of(nr)] = a

    # ------------------------------------------------------------ directories
    def entry(self, e):
        name = e['name'].encode('latin1')[:FNSIZE - 1]
        comment = e['comment'].encode('latin1')[:CMSIZE - 1]
        base = (20 + len(name) + len(comment)) & ~1
        extra = []
        prot = e['protect'] & 0xffffff00
        if e['kind'] == 'hardlink':                             # the object's anode
            t = e['target']
            link = self.dir_anode[t] if t in self.dir_anode else self.file_anode[t]
        else:                                                   # the object's first link
            link = self.first_link.get(e['idx'], 0)
        words = [link >> 16, link & 0xffff, 0, 0, prot >> 16, prot & 0xffff, 0, 0, 0, 0, 0]
        flags = 0
        for i, w in enumerate(words):
            if w:
                extra.append(w)
                flags |= 1 << i
        size = base + 2 * len(extra) + 2
        b = bytearray(size)
        if e['kind'] == 'dir':
            typ, anode, fsize = ST_USERDIR, self.dir_anode[e['idx']], 0
        elif e['kind'] == 'file':
            typ, anode, fsize = ST_FILE, self.file_anode[e['idx']], e['size']
        elif e['kind'] == 'softlink':
            typ, anode, fsize = ST_SOFTLINK, self.file_anode[e['idx']], len(e['link'].encode('latin1'))
        else:
            t = self.body_by_idx[e['target']]
            typ = ST_LINKDIR if t['kind'] == 'dir' else ST_LINKFILE
            anode, fsize = self.file_anode[e['idx']], (t['size'] if t['kind'] == 'file' else 0)
        d = [x & 0xffff for x in e['date']]
        struct.pack_into('>BbIIHHHBB', b, 0, size, typ, anode, fsize, d[0], d[1], d[2],
                         e['protect'] & 0xff, len(name))
        b[18:18 + len(name)] = name
        b[18 + len(name)] = len(comment)
        b[19 + len(name):19 + len(name) + len(comment)] = comment
        p = base
        for w in reversed(extra):
            struct.pack_into('>H', b, p, w)
            p += 2
        struct.pack_into('>H', b, p, flags)
        return bytes(b)

    def _dirs(self, idx, children, depth):
        if depth > 200:
            raise BuildError("directories nested too deep")
        head = self.dir_anode[idx]
        parent_anode = 0 if idx == -1 else self.dir_anode[self.parent_of[idx]]
        space = self.rbs - 20
        blocks, cur, used = [], [], 0
        for e in children.get(idx, []):
            de = self.entry(e)
            if used + len(de) + 1 > space:
                blocks.append(cur)
                cur, used = [], 0
            cur.append(de)
            used += len(de)
        blocks.append(cur)
        prev = None
        for i, ents in enumerate(blocks):
            s, b = self.alloc()
            struct.pack_into('>HHIHHII', b, 0, DBLKID, 0, DATESTAMP, 0, 0, head, parent_anode)
            p = 20
            for de in ents:
                b[p:p + len(de)] = de
                p += len(de)
            nr = head if i == 0 else self.new_anode(None)
            self.set_anode(nr, (1, s, 0))
            if prev is not None:
                pc, pb, _ = self.get_anode(prev)
                self.set_anode(prev, (pc, pb, nr))
            prev = nr
        for e in children.get(idx, []):
            if e['kind'] == 'dir':
                self._dirs(e['idx'], children, depth + 1)

    @property
    def parent_of(self):
        if not hasattr(self, '_parent_of'):
            self._parent_of = {e['idx']: e['parent'] for e in self.body['entries']}
        return self._parent_of

    # ------------------------------------------------------------ root and extension
    def _rext(self, b):
        body = self.body
        struct.pack_into('>HHIII', b, 0, EXTENSIONID, 0, 0, DATESTAMP, PFS2VERSION)
        struct.pack_into('>HHH', b, 16, *[x & 0xffff for x in body['rootdate']])
        struct.pack_into('>HHH', b, 22, *[x & 0xffff for x in body['voldate_alt']])
        struct.pack_into('>I', b, 44, self.next_res)             # reserved roving
        struct.pack_into('>HHHHH', b, 48, 0, self.curranseqnr & 0xffff, 0, DELDIR_BLOCKS, FNSIZE)
        for i, s in enumerate(self.superindex):
            struct.pack_into('>I', b, 64 + 4 * i, s)
        for i, s in enumerate(self.deldir):
            struct.pack_into('>I', b, 144 + 4 * i, s)

    def _rootblock(self, rext_s):
        body = self.body
        r = bytearray(self.rblkcluster * BS)
        name = body['volname'].encode('latin1')[:30]
        reserved_free = self.numreserved - self.next_res
        options = OPTIONS | (MODE_SUPERINDEX if self.supermode else 0)
        struct.pack_into('>III', r, 0, ID_PFS1, options, DATESTAMP)
        struct.pack_into('>HHHH', r, 12, *[x & 0xffff for x in body['voldate']], 0xf0)
        r[20] = len(name)
        r[21:21 + len(name)] = name
        struct.pack_into('>IIIHHIIIIII', r, 52, self.end - 1, self.first, reserved_free, self.rbs,
                         self.rblkcluster, self.blocksfree, (self.n - self.end) // 20, 0, 0, self.n, rext_s)
        for i, s in enumerate(self.bmindex):
            struct.pack_into('>I', r, 96 + 4 * i, s)
        if not self.supermode:
            for i, s in enumerate(self.index):
                struct.pack_into('>I', r, 116 + 4 * i, s)
        # reserved bitmap behind the rootblock: bit set = free
        struct.pack_into('>HHII', r, 512, BMBLKID, 0, DATESTAMP, 0)
        for i in range(self.numreserved // 32):
            v = 0
            for j in range(32):
                if i * 32 + j >= self.next_res:
                    v |= 0x80000000 >> j
            struct.pack_into('>I', r, 524 + 4 * i, v)
        return bytes(r)


def reserved_blocks(body):
    return Builder(body).build()


def softlink_blocks(body):
    """[(block, bytes)]: the data block of every soft link, holding its path."""
    out = []
    for e in body['entries']:
        if e['kind'] == 'softlink':
            p = e['link'].encode('latin1')
            out.append((e['pfs_extents'][0][0], p + bytes(BS - len(p))))
    return out
