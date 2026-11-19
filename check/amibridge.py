"""AmiFUSE's HandlerBridge for the checks: a real Amiga filesystem handler
(m68k emulation) on an image, without FUSE. Adjustments, here only (AmiFUSE
itself is not changed): no real-time sleep for handler timer requests,
directory listings not stopped at 256 entries. Adds the link packets."""
import sys, types, zlib
from pathlib import Path
AF = Path.home() / 'git/AmiFUSE'
sys.path[:0] = [str(AF / 'amitools'), str(AF)]
_fake = types.ModuleType('fuse')
class _D: pass
class _E(RuntimeError): pass
_fake.FUSE = object; _fake.FuseOSError = _E; _fake.LoggingMixIn = _D; _fake.Operations = _D
sys.modules.setdefault('fuse', _fake)
from amifuse.fuse_fs import HandlerBridge, extract_embedded_driver
import time as _time, amitools.vamos.lib.TimerDevice as _TD
import inspect as _insp, textwrap as _tw, amifuse.fuse_fs as _ff


class _NoSleepTime:
    def __getattr__(self, k): return getattr(_time, k)
    def sleep(self, s): pass


_TD.time = _NoSleepTime()
_src = _tw.dedent(_insp.getsource(_ff.HandlerBridge.list_dir)).replace('range(256)', 'range(10**7)')
_ns = {}
exec(compile(_src, _ff.__file__, 'exec'), _ff.__dict__, _ns)
HandlerBridge.list_dir = _ns['list_dir']

ACTION_MAKE_LINK, ACTION_READ_LINK = 1021, 1024
LINK_HARD, LINK_SOFT = 0, 1


def packet(b, ptype, args):
    b.launcher.send_packet(b.state, ptype, args)
    r = b._run_until_replies()
    return (r[-1][2], r[-1][3]) if r else (0, -1)


def cstr(b, s, size=None):
    data = s.encode('latin-1') + b'\0'
    m = b.vh.alloc.alloc_memory(max(len(data), size or 0), label='CSTR')
    b.mem.w_block(m.addr, data + bytes(max(0, (size or 0) - len(data))))
    return m


def split(path):
    parts = [p for p in path.split('/') if p]
    return '/'.join(parts[:-1]), parts[-1]


def make_link(b, linkpath, target, soft):
    """Create linkpath -> target (hard: an existing object path; soft: any path string)."""
    d, name = split(linkpath)
    dlock, _ = b.locate(0, d)
    _, nb = b._alloc_bstr(name)
    if soft:
        m = cstr(b, target)
        r = packet(b, ACTION_MAKE_LINK, [dlock, nb, m.addr, LINK_SOFT])
    else:
        tlock, _ = b.locate(0, target.strip('/'))
        r = packet(b, ACTION_MAKE_LINK, [dlock, nb, tlock, LINK_HARD]) if tlock else (0, -1)
        b.free_lock(tlock)
    b.free_lock(dlock)
    return r


def read_link(b, linkpath):
    """The path a soft link holds, or None."""
    d, name = split(linkpath)
    dlock, _ = b.locate(0, d)
    m = cstr(b, name)
    buf = cstr(b, '', 1024)
    r1, r2 = packet(b, ACTION_READ_LINK, [dlock, m.addr, buf.addr, 1024])
    b.free_lock(dlock)
    if r1 in (0, 0xffffffff) or r1 >= 1024:
        return None
    return bytes(b.mem.r_block(buf.addr, r1)).decode('latin-1')


def read_all(b, path, size):
    return b''.join(b.read_file(path, min(1 << 18, size - o), o) for o in range(0, size, 1 << 18))


def rdb_driver(img, partition=None):
    res = extract_embedded_driver(Path(img), None, partition)
    return (str(res[0]), zlib.crc32(open(res[0], 'rb').read())) if res else (None, None)
