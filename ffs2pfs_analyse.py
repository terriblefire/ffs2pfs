#!/usr/bin/env python3
"""ffs2pfs_analyse.py IMAGE [PARTITION]

Read-only first step of an in-place FFS -> PFS3 conversion. For each FFS
partition in an RDB disk image (or the one named), walk the filesystem and
report whether it can be converted:

  - files, directories, links; used/free blocks; bitmap consistency
    (blocks claimed twice, in use but marked free, marked used but unowned)
  - the PFS3 layout pfs3aio would create on this partition (format.c rules):
    reserved area size, where it lies
  - FFS blocks inside that reserved area: data blocks that must be moved
    out first, and FFS metadata (captured in the plan, then not needed)
  - free blocks left for relocation targets and the plan/journal, and the
    verdict

Nothing is written. Block numbers in the report are partition-relative.
"""
import collections, struct, sys

BS = 512
HT = 72                                   # hash table / block table size for 512-byte blocks
ST_ROOT, ST_USERDIR, ST_SOFTLINK, ST_LINKDIR, ST_FILE, ST_LINKFILE = 1, 2, 3, 4, -3, -4
T_HEADER, T_LIST = 2, 16


def u32(b, i):
    return struct.unpack_from('>I', b, i * 4)[0]


def s32(b, i):
    return struct.unpack_from('>i', b, i * 4)[0]


def csum(b):
    return sum(struct.unpack('>128I', b)) & 0xffffffff


# ---------------------------------------------------------------- RDB
def read_rdb(img):
    for blk in range(16):
        b = img[blk * BS:(blk + 1) * BS]
        if b[:4] == b'RDSK':
            break
    else:
        sys.exit("no RDSK block in the first 16 blocks")
    parts = []
    nxt = u32(b, 7)
    while nxt != 0xffffffff:
        p = img[nxt * BS:(nxt + 1) * BS]
        if p[:4] != b'PART':
            break
        name = p[37:37 + p[36]].decode('latin1')
        env = [u32(p, 32 + i) for i in range(20)]
        surf, bpt, lo, hi, dt = env[3], env[5], env[9], env[10], env[16]
        reserved = env[6]
        start = lo * surf * bpt
        n = (hi - lo + 1) * surf * bpt
        parts.append(dict(name=name, start=start, blocks=n, dostype=dt, reserved=reserved,
                          surfaces=surf, bpt=bpt, lowcyl=lo, highcyl=hi, bootpri=s32(p, 32 + 15)))
        nxt = u32(p, 4)
    return parts


# ---------------------------------------------------------------- FFS walk
class FFS:
    def __init__(self, img, part):
        self.img, self.p = img, part
        self.n = part['blocks']
        self.base = part['start']
        self.root = (self.n - 1 + part['reserved']) // 2
        self.owner = collections.defaultdict(list)    # block -> [(path, kind)]
        self.kind = {}                               # block -> header/ext/dirdata/data/bitmap/bmext/root/boot
        self.problems = []
        self.files = self.dirs = self.links = 0
        self.data_blocks = 0
        self.extents = 0                             # contiguous data runs over all files
        self.names = 0
        self.comment_bytes = 0

    def B(self, k):
        if not (0 <= k < self.n):
            return None
        o = (self.base + k) * BS
        return self.img[o:o + BS]

    def claim(self, k, path, kind):
        self.owner[k].append(path)
        self.kind.setdefault(k, kind)

    def header(self, k, path):
        b = self.B(k)
        if b is None:
            self.problems.append("%s: header pointer %d out of range" % (path, k)); return None
        if u32(b, 0) != T_HEADER or u32(b, 1) != k or csum(b) != 0:
            self.problems.append("%s: block %d is not a valid header (type %d key %d checksum %s)" % (
                path, k, u32(b, 0), u32(b, 1), "ok" if csum(b) == 0 else "bad")); return None
        return b

    def name(self, b):
        return b[433:433 + min(b[432], 30)].decode('latin1', 'replace')

    def is_root(self, k):
        rb = self.B(k)
        return rb is not None and u32(rb, 0) == T_HEADER and s32(rb, 127) == ST_ROOT and csum(rb) == 0

    def walk(self):
        """False when no valid root: the middle of the partition (where FFS
        looks), or the boot block's root pointer (left by FFS grow tools)."""
        if not self.is_root(self.root):
            stored = u32(self.B(0), 2)
            if self.is_root(stored):
                self.problems.append("root at %d (boot block pointer), not the middle %d: grown partition" % (stored, self.root))
                self.root = stored
            else:
                return False
        rb = self.B(self.root)
        self.claim(0, "boot", "boot"); self.claim(1, "boot", "boot")
        self.claim(self.root, "/", "root")
        self.volname = self.name(rb)
        self.walk_dir(self.root, "")
        self.read_bitmap(rb)
        return True

    def walk_dir(self, k, path):
        b = self.B(k)
        self.dirs += 1
        for i in range(HT):
            e, guard = u32(b, 6 + i), 0
            while e:
                guard += 1
                if guard > 100000:
                    self.problems.append("%s: hash chain %d loops" % (path, i)); break
                h = self.header(e, "%s (hash %d)" % (path or "/", i))
                if h is None:
                    break
                p = path + "/" + self.name(h)
                self.claim(e, p, "header")
                self.names += 1
                self.comment_bytes += h[328 + 0] if h[328] <= 79 else 0
                sec = s32(h, 127)
                if sec == ST_USERDIR:
                    self.walk_dir(e, p)
                elif sec == ST_FILE:
                    self.files += 1
                    self.walk_file(e, h, p)
                elif sec in (ST_SOFTLINK, ST_LINKDIR, ST_LINKFILE):
                    self.links += 1
                else:
                    self.problems.append("%s: unknown sec type %d" % (p, sec))
                e = u32(h, 124)

    def walk_file(self, k, h, p):
        blocks = []
        hdr, guard = h, 0
        while True:
            cnt = u32(hdr, 2)
            if cnt > HT:
                self.problems.append("%s: block count %d > %d" % (p, cnt, HT)); break
            blocks += [u32(hdr, 6 + HT - 1 - i) for i in range(cnt)]
            ext = u32(hdr, 126)
            if not ext:
                break
            guard += 1
            e = self.B(ext)
            if guard > 200000 or e is None or u32(e, 0) != T_LIST or u32(e, 1) != ext or csum(e) != 0:
                self.problems.append("%s: bad extension block %d" % (p, ext)); break
            self.claim(ext, p, "ext")
            hdr = e
        size = u32(h, 81)
        need = (size + BS - 1) // BS
        if len(blocks) != need:
            self.problems.append("%s: %d data blocks for %d bytes (want %d)" % (p, len(blocks), size, need))
        prev = None
        for d in blocks:
            if not (0 < d < self.n):
                self.problems.append("%s: data block %d out of range" % (p, d)); continue
            self.claim(d, p, "data")
            if prev is None or d != prev + 1:
                self.extents += 1
            prev = d
        self.data_blocks += len(blocks)

    def read_bitmap(self, rb):
        if u32(rb, 78) != 0xffffffff:
            self.problems.append("root: bitmap flag says NOT valid (needs validation)")
        bm = [u32(rb, 79 + i) for i in range(25) if u32(rb, 79 + i)]
        ext = u32(rb, 104)
        while ext:
            eb = self.B(ext)
            if eb is None:
                self.problems.append("bitmap extension %d out of range" % ext); break
            self.claim(ext, "bitmap", "bmext")
            bm += [u32(eb, i) for i in range(127) if u32(eb, i)]
            ext = u32(eb, 127)
        self.free = set()
        bit = 0
        first = self.p['reserved']
        for k in bm:
            b = self.B(k)
            self.claim(k, "bitmap", "bitmap")
            if csum(b) != 0:
                self.problems.append("bitmap block %d: checksum bad" % k)
            for i in range(1, 128):
                v = u32(b, i)
                for j in range(32):
                    blk = first + bit
                    if blk < self.n and v & (1 << j):
                        self.free.add(blk)
                    bit += 1
        self.bitmap_blocks = len(bm)


# ---------------------------------------------------------------- PFS3 layout (pfs3aio format.c)
MAXNUMRESERVED = 4096 + 255 * 1024 * 8
MAXSMALLDISK = 5 * 253 * 253 * 32
MAXDISKSIZE1K = 104 * 253 * 253 * 32
MAXDISKSIZE2K = 104 * 509 * 509 * 32


def pfs3_layout(total):
    resblocksize = 1024
    if total > MAXSMALLDISK:
        resblocksize = 2048 if total <= MAXDISKSIZE2K else 4096
        if total <= MAXDISKSIZE1K:
            resblocksize = 1024
    taken, i = 32, 2048
    while i and i // 2 < total:
        m = 10 if i >= 512 * 2048 else 14
        taken += taken * m // 16
        i <<= 1
        if i > 0xffffffff:
            break
    taken //= resblocksize // 1024
    taken = min(MAXNUMRESERVED, taken - 1)
    numreserved = (taken + 31) & ~0x1f
    rescluster = resblocksize // BS
    end = rescluster * numreserved + 2                # ROOTBLOCK = 2
    return dict(resblocksize=resblocksize, numreserved=numreserved, first=2, end=end,
                supermode=total > MAXSMALLDISK)


# ---------------------------------------------------------------- report
def analyse(img, part):
    dt = part['dostype']
    tag = struct.pack('>I', dt)
    print("=" * 78)
    print("%s:%s blocks %d-%d of the disk (%d blocks, %.1f MB), DosType %s\\%d" % (
        part['name'], "", part['start'], part['start'] + part['blocks'] - 1, part['blocks'],
        part['blocks'] * BS / 1048576, tag[:3].decode('latin1'), tag[3]))
    if tag[:3] != b'DOS':
        print("  not an FFS/OFS partition: skipped"); return
    if tag[3] in (0, 2, 4):
        print("  OFS (data blocks carry headers): cannot be converted in place"); return
    if tag[3] > 7:
        print("  unknown DOS type: skipped"); return
    if tag[3] in (6, 7):
        print("  note: long-filename FFS (DOS\\%d); names longer than PFS3 allows would need checking" % tag[3])
    f = FFS(img, part)
    if not f.walk():
        print("  no valid FFS root (not at %d, the middle, nor at %d, the boot block's pointer):" % (f.root, u32(f.B(0), 2)))
        print("  not a working FFS volume (unformatted, or the root is lost): NOT convertible")
        return
    owned = set(f.owner)
    dup = [k for k, v in f.owner.items() if len(v) > 1]
    used_but_free = sorted(k for k in owned if k in f.free)
    leaked = [k for k in range(part['reserved'], f.n) if k not in f.free and k not in owned]
    nfree = len(f.free - owned)
    meta = sum(1 for k, v in f.kind.items() if v in ("header", "ext", "bitmap", "bmext", "root"))
    print("  volume \"%s\": %d dirs, %d files, %d links; %d data blocks in %d extents" % (
        f.volname, f.dirs, f.files, f.links, f.data_blocks, f.extents))
    print("  blocks: %d data, %d FFS metadata, %d free (bitmap), bitmap in %d blocks" % (
        f.data_blocks, meta, nfree, f.bitmap_blocks))
    print("  consistency: %d blocks claimed twice, %d in use but marked free, %d marked used but unowned, %d other problems" % (
        len(dup), len(used_but_free), len(leaked), len(f.problems)))
    for pr in f.problems[:10]:
        print("    - " + pr)
    if dup:
        print("    - e.g. block %d: %s" % (dup[0], " | ".join(f.owner[dup[0]][:3])))

    lay = pfs3_layout(f.n)
    a, b = lay['first'], lay['end']
    print("  PFS3 (pfs3aio rules): %d reserved blocks of %d bytes -> blocks %d-%d (%d blocks, %.1f MB)%s" % (
        lay['numreserved'], lay['resblocksize'], a, b - 1, b - a, (b - a) * BS / 1048576,
        ", supermode" if lay['supermode'] else ""))
    inside = [k for k in range(a, b) if k in owned]
    inside_data = [k for k in inside if f.kind[k] == "data"]
    inside_meta = [k for k in inside if f.kind[k] != "data"]
    files_hit = collections.Counter(f.owner[k][0] for k in inside_data)
    free_outside = sorted(k for k in f.free - owned if not (a <= k < b))
    print("  in the PFS3 reserved area: %d FFS blocks in use: %d data (to move, from %d files), %d FFS metadata (dropped)" % (
        len(inside), len(inside_data), len(files_hit), len(inside_meta)))
    for path, n in files_hit.most_common(5):
        print("    %6d blocks of %s" % (n, path))
    # plan size: per entry ~64 bytes + name + comment, per extent 8 bytes
    plan_bytes = (f.names + 1) * 64 + f.names * 31 + f.comment_bytes + f.extents * 8 + len(inside_data) * 8
    plan_blocks = (plan_bytes + BS - 1) // BS
    journal_blocks = 2 + 64                          # two headers + progress log
    need = len(inside_data) + plan_blocks * 2 + journal_blocks   # plan kept twice
    print("  plan ~%d KB (%d blocks, kept twice), journal %d blocks" % (plan_bytes // 1024, plan_blocks, journal_blocks))
    print("  free blocks outside the reserved area: %d; needed: %d (moves %d + plan %d + journal %d)" % (
        len(free_outside), need, len(inside_data), plan_blocks * 2, journal_blocks))
    after = nfree + meta - (b - a) + len(inside_meta)
    print("  after conversion: about %d free blocks (%.1f MB) on PFS3 (FFS metadata freed, reserved area taken)" % (
        after, after * BS / 1048576))
    ok = len(free_outside) >= need and not dup and not used_but_free
    why = []
    if len(free_outside) < need:
        why.append("not enough free space (%d short)" % (need - len(free_outside)))
    if dup:
        why.append("blocks claimed twice: repair the FFS volume first")
    if used_but_free:
        why.append("bitmap marks used blocks free: validate/repair first")
    print("  verdict: %s" % ("can be converted" if ok else "NOT convertible: " + "; ".join(why)))


def main():
    img = open(sys.argv[1], 'rb').read()
    want = sys.argv[2] if len(sys.argv) > 2 else None
    parts = read_rdb(img)
    print("%s: %d partitions: %s" % (sys.argv[1], len(parts), ", ".join(p['name'] for p in parts)))
    for p in parts:
        if want is None or p['name'].lower() == want.lower():
            analyse(img, p)


if __name__ == '__main__':
    main()
