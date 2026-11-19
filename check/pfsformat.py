#!/usr/bin/env python3
"""Format partition 0 of an RDB image with the real pfs3aio handler (AmiFUSE)."""
import sys, types
from pathlib import Path
AF = Path.home() / 'git/AmiFUSE'
sys.path[:0] = [str(AF / 'amitools'), str(AF)]
fake = types.ModuleType('fuse')
class _D: pass
class _E(RuntimeError): pass
fake.FUSE = object; fake.FuseOSError = _E; fake.LoggingMixIn = _D; fake.Operations = _D
sys.modules['fuse'] = fake
from amifuse.fuse_fs import format_volume
format_volume(Path(sys.argv[1]), Path(sys.argv[2]), None, sys.argv[3] if len(sys.argv) > 3 else 'DH0', sys.argv[4] if len(sys.argv) > 4 else 'Test')
