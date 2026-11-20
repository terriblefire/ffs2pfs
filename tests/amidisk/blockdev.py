"""Block device over a disk image, with a write overlay and crash injection.

Every 512-byte block written counts as one write. With crash_after=k the
k+1-th block write raises Crash before anything is written, so a multi-block
write can be cut part way, as a power cut would. Writes go to an overlay dict
(absolute block -> bytes) unless the device was opened writable on the file.
"""
BS = 512


class Crash(Exception):
    pass


class BlockDev:
    def __init__(self, path, overlay=None, crash_after=None, writable_file=False, torn=False):
        self.path = path
        self.f = open(path, 'r+b' if writable_file else 'rb')
        self.f.seek(0, 2)
        self.nblocks = self.f.tell() // BS
        self.overlay = {} if overlay is None else overlay
        self.writable_file = writable_file
        self.crash_after = crash_after
        self.torn = torn                 # None/False, an offset (first bytes new, rest old) or 'garbage'
        self.writes = 0

    def read(self, blk, n=1):
        out = bytearray()
        for k in range(blk, blk + n):
            d = self.overlay.get(k)
            if d is None:
                self.f.seek(k * BS)
                d = self.f.read(BS)
            out += d
        return bytes(out)

    def write(self, blk, data):
        assert len(data) % BS == 0
        for i in range(len(data) // BS):
            b = bytes(data[i * BS:(i + 1) * BS])
            if self.crash_after is not None and self.writes >= self.crash_after:
                if self.torn == 'garbage':
                    import random
                    self._put(blk + i, bytes(random.Random(blk + i).getrandbits(8) for _ in range(BS)))
                elif self.torn:
                    off = BS // 2 if self.torn is True else int(self.torn)
                    old = self.read(blk + i)
                    self._put(blk + i, b[:off] + old[off:])
                raise Crash(self.writes)
            self._put(blk + i, b)
            self.writes += 1

    def _put(self, k, b):
        if self.writable_file:
            self.f.seek(k * BS)
            self.f.write(b)
        else:
            self.overlay[k] = b

    def close(self):
        self.f.close()


class Part:
    """A partition: block numbers relative to its first block."""

    def __init__(self, dev, start, nblocks):
        self.dev, self.start, self.n = dev, start, nblocks

    def read(self, k, n=1):
        assert 0 <= k and k + n <= self.n, (k, n, self.n)
        return self.dev.read(self.start + k, n)

    def write(self, k, data):
        assert 0 <= k and k + len(data) // BS <= self.n, (k, len(data), self.n)
        self.dev.write(self.start + k, data)
