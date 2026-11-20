"""Check a finished conversion: read the partition back as PFS3 (pfs3read) and
compare every entry and every file's contents with the untouched FFS volume
(reference())."""
import zlib
from . import rdb as rdbmod
from .blockdev import Part, BS
from .ffs import FFS
from .pfs3read import PFS3, ST_FILE, ST_USERDIR, ST_SOFTLINK, ST_LINKFILE, ST_LINKDIR
from . import rdbfs
from .rdbfs import DOSTYPE_PFS3

BOOT0 = b'PFS\x01' + bytes(BS - 4)      # the PFS3 boot block


def reference(part):
    """what verify() compares with: the entries, file CRCs and volume name of the FFS volume"""
    ffs = FFS(part).read()
    return dict(entries=ffs.entries, volname=ffs.volname,
                crcs={e['idx']: ffs.file_crc(e) for e in ffs.entries if e['kind'] == 'file'})


def ffs_paths(body):
    by = {e['idx']: e for e in body['entries']}
    out = {}
    for e in body['entries']:
        names, x = [], e
        while x is not None:
            names.append(x['name'])
            x = by.get(x['parent'])
        out['/' + '/'.join(reversed(names))] = e
    return out


def verify(dev, pname, body, strict_boot1=True, binary=None):
    errs = list(rdbfs.check(dev, binary))
    p = [x for x in rdbmod.partitions(dev) if not x['invalid'] and x['name'] == pname][0]
    if p['dostype'] != DOSTYPE_PFS3:
        errs.append("RDB DosType %08x, not PFS\\3" % p['dostype'])
    part = Part(dev, p['start'], p['blocks'])
    if part.read(0) != BOOT0:
        errs.append("block 0 is not the PFS3 boot block")
    if strict_boot1 and any(part.read(1)):
        errs.append("block 1 not zero")
    try:
        fs = PFS3(part).read()
    except Exception as e:
        return errs + ["PFS3 read failed: %r" % e]
    errs += fs.problems[:20]
    want = ffs_paths(body)
    paths_by_idx = {e['idx']: p for p, e in want.items()}
    by_idx = {e['idx']: e for e in body['entries']}
    got = {e['path']: e for e in fs.entries}
    if set(want) != set(got):
        errs.append("entries differ: missing %s, extra %s" % (sorted(set(want) - set(got))[:3],
                                                             sorted(set(got) - set(want))[:3]))
    for path, w in want.items():
        g = got.get(path)
        if not g:
            continue
        want_type = {'dir': (ST_USERDIR,), 'file': (ST_FILE,), 'softlink': (ST_SOFTLINK,),
                     'hardlink': (ST_LINKFILE, ST_LINKDIR)}[w['kind']]
        if g['type'] not in want_type:
            errs.append("%s: type %d" % (path, g['type']))
            continue
        if w['kind'] == 'softlink' and g.get('target') != w['link']:
            errs.append("%s: soft link to %r, want %r" % (path, g.get('target'), w['link']))
        if w['kind'] == 'hardlink':
            tpath = paths_by_idx[w['target']]
            if g.get('target_path') != tpath:
                errs.append("%s: hard link to %s, want %s" % (path, g.get('target_path'), tpath))
            if g['type'] != (ST_LINKDIR if by_idx[w['target']]['kind'] == 'dir' else ST_LINKFILE):
                errs.append("%s: link type %d" % (path, g['type']))
        prot = g['prot'] | (g['extra'].get('prot_hi', 0) << 16) | g['extra'].get('prot_lo', 0)
        if prot != w['protect'] or list(g['date']) != [x & 0xffff for x in w['date']] or g['comment'] != w['comment']:
            errs.append("%s: attributes differ" % path)
        if w['kind'] == 'file':
            if g['fsize'] != w['size']:
                errs.append("%s: size %d, want %d" % (path, g['fsize'], w['size']))
                continue
            c, left = 0, w['size']
            for cs, blk in g['chain']:
                for k in range(blk, blk + cs):
                    c = zlib.crc32(part.read(k)[:min(BS, left)], c)
                    left -= BS
            if c != body['crcs'][w['idx']]:
                errs.append("%s: contents differ" % path)
    if fs.root['diskname'] != body['volname']:
        errs.append("volume name %r" % fs.root['diskname'])
    return errs
