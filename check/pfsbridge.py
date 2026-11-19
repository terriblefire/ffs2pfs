#!/usr/bin/env python3
"""pfsbridge.py IMAGE DRIVER|rdb [PARTITION] - walk a volume with a real Amiga
filesystem handler (AmiFUSE) and print JSON: path -> 'dir' | [size, crc] (files,
and files read through hard links) | 'linkdir' | ['soft', path]."""
import json, os, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from amibridge import HandlerBridge, Path, read_link, read_all, rdb_driver


def walk(b):
    out, stack = {}, ['/']
    while stack:
        cur = stack.pop()
        for e in b.list_dir_path(cur):
            p = '/' + e['name'] if cur == '/' else cur + '/' + e['name']
            t = e['dir_type']
            if t in (1, 2):
                out[p] = 'dir'; stack.append(p)
            elif t == 4:
                out[p] = 'linkdir'                       # never followed: it may point upwards
            elif t == 3:
                out[p] = ['soft', read_link(b, p)]
            else:
                size = int(e.get('size', 0))
                out[p] = [size, zlib.crc32(read_all(b, p, size))]
    return out


if __name__ == '__main__':
    img, drv = sys.argv[1], sys.argv[2]
    drv_crc = None
    if drv == 'rdb':
        drv, drv_crc = rdb_driver(img, sys.argv[3] if len(sys.argv) > 3 else None)
        if not drv:
            print("no filesystem in the RDB for this partition", file=sys.stderr); sys.exit(3)
    b = HandlerBridge(Path(img), Path(drv), **({'partition': sys.argv[3]} if len(sys.argv) > 3 else {}))
    out = walk(b)
    out['__driver_crc__'] = drv_crc
    json.dump(out, sys.stdout, indent=0, sort_keys=True)
