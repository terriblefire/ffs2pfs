import sys, types
from pathlib import Path
AF = Path.home() / 'git/AmiFUSE'
sys.path[:0] = [str(AF / 'amitools'), str(AF)]
fake = types.ModuleType('fuse')
class _D: pass
class _E(RuntimeError): pass
fake.FUSE = object; fake.FuseOSError = _E; fake.LoggingMixIn = _D; fake.Operations = _D
sys.modules['fuse'] = fake
from amifuse.fuse_fs import HandlerBridge
import time as _time, amitools.vamos.lib.TimerDevice as _TD
class _NoSleepTime:                       # handler timer requests: no real-time wait
    def __getattr__(self, k): return getattr(_time, k)
    def sleep(self, s): pass
_TD.time = _NoSleepTime()
import inspect as _insp, textwrap as _tw, amifuse.fuse_fs as _ff
_src = _tw.dedent(_insp.getsource(_ff.HandlerBridge.list_dir)).replace('range(256)', 'range(10**7)')
_ns = {}
exec(compile(_src, _ff.__file__, 'exec'), _ff.__dict__, _ns)
HandlerBridge.list_dir = _ns['list_dir']          # AmiFUSE stops a listing at 256 entries
b = HandlerBridge(Path(sys.argv[1]), Path(sys.argv[2]), debug='-d' in sys.argv)
print("mounted", flush=True)
for e in b.list_dir_path(sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith('-') else '/'):
    print(e['name'], e['dir_type'], e.get('size'), flush=True)
import zlib, time
for path in sys.argv[4:]:
    if path.startswith('-'): continue
    t = time.time()
    if path.endswith('/'):
        print(path, [e['name'] for e in b.list_dir_path(path.rstrip('/'))], flush=True)
    else:
        st = [e for e in b.list_dir_path('/' + '/'.join(path.strip('/').split('/')[:-1])) if e['name'] == path.split('/')[-1]][0]
        data = b.read_file(path, st['size'], 0)
        print(path, len(data), hex(zlib.crc32(data)), "%.1fs" % (time.time() - t), flush=True)
