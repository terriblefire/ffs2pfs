"""PFS3 (pfs3aio, harddisk mode, split anodes) reader and consistency checker.

Reads a partition through a Part view and checks: block ids and seqnrs, the
anode tree, the directory tree, the reserved bitmap against every reserved
block in use, and the data bitmap against every data block in use.
"""
import struct

BS = 512
ID_PFS1, ID_PFS2 = 0x50465301, 0x50465302
DBLKID, ABLKID, IBLKID, BMBLKID, BMIBLKID, DELDIRID, EXTENSIONID, SBLKID = (
    0x4442, 0x4142, 0x4942, 0x424D, 0x4D49, 0x4444, 0x4558, 0x5342)
MODE_SUPERINDEX = 128
ST_FILE, ST_USERDIR, ST_SOFTLINK, ST_LINKDIR, ST_LINKFILE = -3, 2, 3, 4, -4
EXTRA_FIELDS = ('link_hi', 'link_lo', 'uid', 'gid', 'prot_hi', 'prot_lo',
                'vsize_hi', 'vsize_lo', 'roll_hi', 'roll_lo', 'fsizex')


class PFSError(Exception):
    pass


def be16(b, o):
    return struct.unpack_from('>H', b, o)[0]


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def parse_root(b):
    r = dict(disktype=be32(b, 0), options=be32(b, 4), datestamp=be32(b, 8),
             creation=struct.unpack_from('>HHH', b, 12), protection=be16(b, 18),
             diskname=b[21:21 + b[20]].decode('latin1'),
             lastreserved=be32(b, 52), firstreserved=be32(b, 56), reserved_free=be32(b, 60),
             reserved_blksize=be16(b, 64), rblkcluster=be16(b, 66), blocksfree=be32(b, 68),
             alwaysfree=be32(b, 72), roving_ptr=be32(b, 76), deldir=be32(b, 80),
             disksize=be32(b, 84), extension=be32(b, 88))
    r['small_bmindex'] = [be32(b, 96 + 4 * i) for i in range(5)]
    r['small_index'] = [be32(b, 116 + 4 * i) for i in range(99)]
    r['large_bmindex'] = [be32(b, 96 + 4 * i) for i in range(104)]
    return r


def parse_rext(b):
    x = dict(id=be16(b, 0), ext_options=be32(b, 4), datestamp=be32(b, 8), pfs2version=be32(b, 12),
             root_date=struct.unpack_from('>HHH', b, 16), volume_date=struct.unpack_from('>HHH', b, 22),
             tobedone=struct.unpack_from('>IIII', b, 28), reserved_roving=be32(b, 44),
             rovingbit=be16(b, 48), curranseqnr=be16(b, 50), deldirroving=be16(b, 52),
             deldirsize=be16(b, 54), fnsize=be16(b, 56))
    x['superindex'] = [be32(b, 64 + 4 * i) for i in range(16)]
    o = 64 + 64
    x['dd_uid'], x['dd_gid'] = be16(b, o), be16(b, o + 2)
    x['dd_protection'] = be32(b, o + 4)
    x['dd_date'] = struct.unpack_from('>HHH', b, o + 8)
    x['deldir'] = [be32(b, o + 16 + 4 * i) for i in range(32)]
    x['dosenvec'] = [be32(b, o + 16 + 128 + 4 * i) for i in range(17)]
    return x


def parse_entries(b, start=20):
    """Directory entries of a dir block."""
    out, o = [], start
    while o < len(b) and b[o]:
        n = b[o]
        if n & 1 or n < 18 or o + n > len(b):
            raise PFSError("bad direntry size %d at %d" % (n, o))
        typ = struct.unpack_from('>b', b, o + 1)[0]
        anode, fsize = be32(b, o + 2), be32(b, o + 6)
        date = struct.unpack_from('>HHH', b, o + 10)
        prot, nlen = b[o + 16], b[o + 17]
        name = b[o + 18:o + 18 + nlen].decode('latin1')
        clen = b[o + 18 + nlen]
        comment = b[o + 19 + nlen:o + 19 + nlen + clen].decode('latin1')
        minoff = (20 + nlen + clen) & ~1
        extra = {}
        if n >= minoff + 2:
            p = o + n
            p -= 2
            flags = be16(b, p)
            for i, f in enumerate(EXTRA_FIELDS):
                if flags & (1 << i):
                    p -= 2
                    extra[f] = be16(b, p)
        out.append(dict(off=o, next=n, type=typ, anode=anode, fsize=fsize, date=date, prot=prot,
                        name=name, comment=comment, extra=extra))
        o += n
    return out


class PFS3:
    def __init__(self, part):
        self.p = part
        self.used_res = {}           # reserved block -> what
        self.used_data = {}          # data block -> owner
        self.problems = []

    def rd(self, blk):
        """One reserved block (rescluster sectors)."""
        return self.p.read(blk, self.rescluster)

    def res(self, blk, what):
        if not (self.root['firstreserved'] <= blk <= self.root['lastreserved']) or \
                (blk - self.root['firstreserved']) % self.rescluster:
            self.problems.append("%s at %d is not a reserved block" % (what, blk))
        if blk in self.used_res:
            self.problems.append("reserved block %d used twice (%s, %s)" % (blk, self.used_res[blk], what))
        self.used_res[blk] = what

    def read(self):
        b0 = self.p.read(0)
        self.boot = be32(b0, 0)
        rb = self.p.read(2)
        self.root = r = parse_root(rb)
        if r['disktype'] not in (ID_PFS1, ID_PFS2):
            raise PFSError("rootblock disktype %08x" % r['disktype'])
        self.rbs = r['reserved_blksize']
        self.rescluster = self.rbs // BS
        self.super = bool(r['options'] & MODE_SUPERINDEX)
        self.rootcluster = self.p.read(2, r['rblkcluster'])
        for i in range(r['rblkcluster'] // self.rescluster):
            self.res(2 + i * self.rescluster, 'rootblock')
        self.anodesper = (self.rbs - 16) // 12
        self.indexper = (self.rbs - 12) // 4
        self.ext = None
        if r['extension']:
            self.res(r['extension'], 'rext')
            eb = self.rd(r['extension'])
            self.ext = parse_rext(eb)
            if self.ext['id'] != EXTENSIONID:
                self.problems.append("rext id %04x" % self.ext['id'])
        self._anodes()
        self._bitmap()
        self._deldir()
        self.entries = []
        self._dir(5, 0, '', 0)
        self._deldir_entries()
        self._check_links()
        self._check_bitmaps()
        return self

    def _deldir_entries(self):
        """Deleted files keep their anode chains (their blocks are free in the bitmap)."""
        self.deleted = []
        for i, b in enumerate(self.deldir):
            for k in range(31):
                o = 32 + 32 * k
                an = be32(b, o)
                if an:
                    name = b[o + 15:o + 15 + min(b[o + 14], 17)].decode('latin1')
                    self.deleted.append((name, an, be32(b, o + 4)))
                    self.chain(an, 'deleted ' + name)

    # ---------------------------------------------------------------- anodes
    def _idxblock(self, blk, want_id, seq, what):
        b = self.rd(blk)
        if be16(b, 0) != want_id or be32(b, 8) != seq:
            self.problems.append("%s %d at %d: id %04x seq %d" % (what, seq, blk, be16(b, 0), be32(b, 8)))
        self.res(blk, '%s %d' % (what, seq))
        return [be32(b, 12 + 4 * i) for i in range(self.indexper)], b

    def _anodes(self):
        self.anodeblocks = {}          # seqnr -> block
        if self.super:
            index_blocks = {}
            for s, sb in enumerate(self.ext['superindex']):
                if sb:
                    idx, _ = self._idxblock(sb, SBLKID, s, 'superblock')
                    for i, ib in enumerate(idx):
                        if ib:
                            index_blocks[s * self.indexper + i] = ib
        else:
            index_blocks = {i: ib for i, ib in enumerate(self.root['small_index']) if ib}
        for n, ib in sorted(index_blocks.items()):
            idx, _ = self._idxblock(ib, IBLKID, n, 'indexblock')
            for i, ab in enumerate(idx):
                if ab:
                    self.anodeblocks[n * self.indexper + i] = ab
        self.anodes = {}
        for seq, ab in sorted(self.anodeblocks.items()):
            b = self.rd(ab)
            if be16(b, 0) != ABLKID or be32(b, 8) != seq:
                self.problems.append("anodeblock %d at %d: id %04x seq %d" % (seq, ab, be16(b, 0), be32(b, 8)))
            self.res(ab, 'anodeblock %d' % seq)
            for k in range(self.anodesper):
                a = struct.unpack_from('>III', b, 16 + 12 * k)
                if a != (0, 0, 0):
                    self.anodes[(seq << 16) | k] = a
        self.anode_refs = {}

    def chain(self, nr, owner):
        out, guard = [], 0
        while nr:
            if nr not in self.anodes:
                self.problems.append("%s: anode %08x missing" % (owner, nr))
                break
            if nr in self.anode_refs:
                self.problems.append("anode %08x used by %s and %s" % (nr, self.anode_refs[nr], owner))
            self.anode_refs[nr] = owner
            cs, blk, nxt = self.anodes[nr]
            out.append((cs, blk))
            nr = nxt
            guard += 1
            if guard > 10000000:
                raise PFSError("anode loop")
        return out

    # ---------------------------------------------------------------- bitmaps
    def _bitmap(self):
        r = self.root
        rbm = self.rootcluster[512:]
        if be16(rbm, 0) != BMBLKID:
            self.problems.append("reserved bitmap id %04x" % be16(rbm, 0))
        nres = (r['lastreserved'] - r['firstreserved'] + 1) // self.rescluster
        self.nres = nres
        self.res_free = set()
        for i in range(nres):
            if be32(rbm, 12 + 4 * (i // 32)) & (0x80000000 >> (i % 32)):
                self.res_free.add(r['firstreserved'] + i * self.rescluster)
        self.longsperbmb = self.rbs // 4 - 3
        self.bitmapstart = r['lastreserved'] + 1
        nbits = self.p.n - self.bitmapstart
        no_bmb = ((nbits + 31) // 32 + self.longsperbmb - 1) // self.longsperbmb
        bmi = r['large_bmindex'] if self.super else r['small_bmindex']
        self.data_free = set()
        self.bitmap_tail_free = True
        for seq in range(no_bmb):
            ii, oo = divmod(seq, self.indexper)
            if not bmi[ii]:
                self.problems.append("bitmap index %d missing" % ii)
                continue
            idx = self._bmi_cache(bmi[ii], ii)
            bb = idx[oo]
            if not bb:
                self.problems.append("bitmap block %d missing" % seq)
                continue
            b = self.rd(bb)
            if be16(b, 0) != BMBLKID or be32(b, 8) != seq:
                self.problems.append("bitmap block %d at %d: id %04x seq %d" % (seq, bb, be16(b, 0), be32(b, 8)))
            self.res(bb, 'bitmap %d' % seq)
            base = seq * self.longsperbmb * 32
            for li in range(self.longsperbmb):
                v = be32(b, 12 + 4 * li)
                for j in range(32):
                    bit = base + li * 32 + j
                    if v & (0x80000000 >> j):
                        if bit < nbits:
                            self.data_free.add(self.bitmapstart + bit)
                    elif bit >= nbits:
                        self.bitmap_tail_free = False

    _bmi = None

    def _bmi_cache(self, blk, n):
        self._bmi = self._bmi or {}
        if n not in self._bmi:
            self._bmi[n], _ = self._idxblock(blk, BMIBLKID, n, 'bitmapindex')
        return self._bmi[n]

    def _deldir(self):
        self.deldir = []
        if not self.ext:
            return
        for i in range(self.ext['deldirsize']):
            blk = self.ext['deldir'][i]
            b = self.rd(blk)
            if be16(b, 0) != DELDIRID or be32(b, 8) != i:
                self.problems.append("deldir %d at %d: id %04x seq %d" % (i, blk, be16(b, 0), be32(b, 8)))
            self.res(blk, 'deldir %d' % i)
            self.deldir.append(b)

    # ---------------------------------------------------------------- directories
    def _dir(self, anode, parent, path, depth):
        if depth > 200:
            raise PFSError("too deep")
        for cs, blk in self.chain(anode, 'dir ' + (path or '/')):
            if cs != 1:
                self.problems.append("dir %s: dir block anode clustersize %d" % (path, cs))
            b = self.rd(blk)
            self.res(blk, 'dirblock ' + (path or '/'))
            if be16(b, 0) != DBLKID or be32(b, 12) != anode or be32(b, 16) != parent:
                self.problems.append("dirblock %d (%s): id %04x anodenr %08x parent %08x, want %08x %08x" % (
                    blk, path, be16(b, 0), be32(b, 12), be32(b, 16), anode, parent))
            for e in parse_entries(b):
                p = path + '/' + e['name']
                e['path'] = p
                e['in_dir'] = anode
                self.entries.append(e)
                if e['type'] == ST_USERDIR:
                    self._dir(e['anode'], anode, p, depth + 1)
                elif e['type'] == ST_FILE:
                    ch = self.chain(e['anode'], p)
                    e['chain'] = ch
                    nblk = sum(cs for cs, _ in ch)
                    if nblk != (e['fsize'] + BS - 1) // BS:
                        self.problems.append("%s: %d blocks for %d bytes" % (p, nblk, e['fsize']))
                    for cs, blk0 in ch:
                        for d in range(blk0, blk0 + cs):
                            if d in self.used_data:
                                self.problems.append("data block %d in %s and %s" % (d, self.used_data[d], p))
                            self.used_data[d] = p
                elif e['type'] == ST_SOFTLINK:
                    ch = self.chain(e['anode'], p)
                    e['chain'] = ch
                    if len(ch) != 1 or ch[0][0] != 1 or e['fsize'] >= BS:
                        self.problems.append("%s: soft link chain %s size %d" % (p, ch, e['fsize']))
                    else:
                        blk = ch[0][1]
                        if blk in self.used_data:
                            self.problems.append("data block %d in %s and %s" % (blk, self.used_data[blk], p))
                        self.used_data[blk] = p
                        e['target'] = self.p.read(blk)[:e['fsize']].decode('latin1')
                elif e['type'] in (ST_LINKFILE, ST_LINKDIR):
                    # the anode is a link node: (object's directory, link's directory, next link)
                    nr = e['anode']
                    if nr not in self.anodes:
                        self.problems.append("%s: link node %08x missing" % (p, nr))
                    else:
                        if nr in self.anode_refs:
                            self.problems.append("anode %08x used by %s and %s" % (nr, self.anode_refs[nr], p))
                        self.anode_refs[nr] = p
                        odir, ldir, nxt = self.anodes[nr]
                        if ldir != anode:
                            self.problems.append("%s: link node says directory %08x, is in %08x" % (p, ldir, anode))
                        e['node'] = (odir, ldir, nxt)
                    e['object'] = (e['extra'].get('link_hi', 0) << 16) | e['extra'].get('link_lo', 0)
                else:
                    self.problems.append("%s: type %d not checked" % (p, e['type']))

    def _check_links(self):
        """Every hard link names an object in the directory its node says, and each
        object's link chain (extra field link, then node.next) holds exactly its links."""
        by_anode = {}
        for e in self.entries:
            if e['type'] in (ST_FILE, ST_USERDIR, ST_SOFTLINK):
                by_anode[e['anode']] = e
        dir_of = {}
        for e in self.entries:
            e['dir_anode'] = e.get('dir_anode')
        links_to = {}
        for e in self.entries:
            if e['type'] in (ST_LINKFILE, ST_LINKDIR) and 'node' in e:
                o = by_anode.get(e['object'])
                if o is None:
                    self.problems.append("%s: link to anode %08x, no such object" % (e['path'], e['object']))
                    continue
                if o['in_dir'] != e['node'][0]:
                    self.problems.append("%s: node says object dir %08x, object is in %08x" % (e['path'], e['node'][0], o['in_dir']))
                if (e['type'] == ST_LINKDIR) != (o['type'] == ST_USERDIR):
                    self.problems.append("%s: link type %d for a %d" % (e['path'], e['type'], o['type']))
                e['target_path'] = o['path']
                links_to.setdefault(o['anode'], set()).add(e['anode'])
        for o in by_anode.values():
            first = (o['extra'].get('link_hi', 0) << 16) | o['extra'].get('link_lo', 0)
            chain, nr = set(), first
            while nr and nr not in chain and len(chain) < 100000:
                chain.add(nr)
                nr = self.anodes.get(nr, (0, 0, 0))[2]
            if chain != links_to.get(o['anode'], set()):
                self.problems.append("%s: link chain %s, links %s" % (o['path'], sorted(chain), sorted(links_to.get(o['anode'], set()))))

    def _check_bitmaps(self):
        r = self.root
        for blk in self.used_res:
            if blk in self.res_free:
                self.problems.append("reserved block %d (%s) used but free in the bitmap" % (blk, self.used_res[blk]))
        allres = {r['firstreserved'] + i * self.rescluster for i in range(self.nres)}
        leaked = allres - set(self.used_res) - self.res_free
        if leaked:
            self.problems.append("%d reserved blocks taken but not used (e.g. %d)" % (len(leaked), min(leaked)))
        if len(self.res_free) != r['reserved_free']:
            self.problems.append("reserved_free %d, bitmap has %d" % (r['reserved_free'], len(self.res_free)))
        for d, who in self.used_data.items():
            if d < self.bitmapstart or d >= self.p.n:
                self.problems.append("%s: data block %d outside the data area" % (who, d))
            elif d in self.data_free:
                self.problems.append("data block %d (%s) used but free in the bitmap" % (d, who))
        alldata = set(range(self.bitmapstart, self.p.n))
        dleak = alldata - set(self.used_data) - self.data_free
        if dleak:
            self.problems.append("%d data blocks taken but not used (e.g. %d)" % (len(dleak), min(dleak)))
        if len(self.data_free) != r['blocksfree']:
            self.problems.append("blocksfree %d, bitmap has %d" % (r['blocksfree'], len(self.data_free)))
        unref = set(self.anodes) - set(self.anode_refs)
        res_anodes = {a for a in unref if (a >> 16) == 0 and (a & 0xffff) < 5}
        if unref - res_anodes:
            self.problems.append("%d anodes in use but not referenced (e.g. %08x)" % (
                len(unref - res_anodes), min(unref - res_anodes)))

    def summary(self):
        r = self.root
        return dict(boot='%08x' % self.boot, disktype='%08x' % r['disktype'], options='%x' % r['options'],
                    datestamp=r['datestamp'], name=r['diskname'], res=(r['firstreserved'], r['lastreserved']),
                    rbs=self.rbs, rblkcluster=r['rblkcluster'], reserved_free=r['reserved_free'],
                    blocksfree=r['blocksfree'], alwaysfree=r['alwaysfree'], extension=r['extension'],
                    super=self.super, anodeblocks=len(self.anodeblocks), anodes=len(self.anodes),
                    entries=len(self.entries), used_res=len(self.used_res), deldir=len(self.deldir))
