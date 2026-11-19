# ffs2pfs

ffs2pfs converts an Amiga OFS or FFS hard disk partition to PFS3 in place, keeping every
file. It runs on AmigaOS (68000 and up) and is built to survive a power cut at any
moment: after one, it either finishes the conversion or puts the partition back to the
FFS volume it was.

- Files, directories, hard and soft links, comments, protection bits and dates are kept.
- The partition's RDB entry gets pfs3aio (the PFS3 handler, built into ffs2pfs) and the
  DosType `PFS\3`, so the disk mounts and boots as PFS3 with no other disk needed.
- Every step is journalled on the disk itself. A conversion that was cut off is found
  again from the disk alone and carries on from where it stopped.
- If anything fails, the volume is rolled back to FFS, exactly as it was.

## Usage

```
ffs2pfs PARTITION/A [ANALYSE] [ROLLBACK] [PFS3AIO=file] [DEVICE=name UNIT=n] [NOREMOUNT] [LOG=file] [BOOT]
```

| command | what it does |
|---|---|
| `ffs2pfs DH1: ANALYSE` | checks the volume and the free space it needs, writes nothing |
| `ffs2pfs DH1:` | converts DH1:, or carries on with a conversion that was cut off |
| `ffs2pfs DH1: ROLLBACK` | puts a conversion that was cut off (power cut, Ctrl-C) back to FFS |
| `DEVICE=scsi.device UNIT=0` | finds the partition on the disk when it is not mounted (a partition being converted has the DosType `F2P\0`, which nothing mounts) |
| `PFS3AIO=file` | puts another pfs3aio into the RDB instead of the built-in 19.2 |
| `NOREMOUNT` | leaves the volume unmounted at the end (otherwise it is mounted as PFS3, write-protected until the next reboot) |
| `LOG=file` | a copy of the messages |
| `BOOT` | converts the boot volume: copy ffs2pfs to RAM:, run it from there, reboot when it is done |

Return codes: 0 done, 5 stopped by Ctrl-C (run it again to carry on, or ROLLBACK),
10 failed but the volume is as it was, 20 error.

The conversion needs free space on the volume for the PFS3 reserved area (FFS data
blocks in it are moved out first), the plan and journal, and the rollback copy.
`ANALYSE` reports how much there is and how much it needs.

## How it works

| step | what is written |
|---|---|
| start | the plan (twice), the journal (twice), an anchor in block 1 |
| lock | an anchor over the boot block, RDB DosType `F2P\0`: from here FFS cannot mount it |
| relocate | FFS: data blocks out of the PFS3 reserved area. OFS: every file packed into free blocks |
| backup | the rollback copy: every block the next two steps overwrite, into free blocks |
| build | the PFS3 metadata in the reserved area |
| RDB | pfs3aio into the RDB |
| commit | block 1 := done mark, block 0 := PFS3 boot block, RDB DosType := `PFS\3` (the point of no return), block 1 := zeros |

Blocks 0 and 1 always hold a mark (anchor, done or undo) that leads to the plan until
the volume is FFS or PFS3 again, and each step ends by reading back what it wrote. A
rollback puts back the rollback copy, the RDB, the DosType and blocks 0 and 1; once
started it always runs to the end, so a power cut during a rollback is handled by
running `ROLLBACK` again. After the point of no return only the last write is left,
and running ffs2pfs again finishes it.

## Building

The Amiga binary needs vbcc and the NDK 3.2; the host build (for the tests) needs gcc:

```
make -C c ffs2pfs NDK32=/path/to/NDK3.2   # c/ffs2pfs: the AmigaOS binary, with ref/pfs3aio built in
make -C c f2p_host                         # c/f2p_host: the same converter on a disk image file
```

`c/` is the converter (C). `f2p/` is the Python prototype it was ported from, which the
tests also use to read FFS and PFS3 images. `ffs2pfs_analyse.py` is a read-only
analysis of an image. `check/` reads converted images with the real pfs3aio handler
(through AmiFUSE). `tools/amiga/crcwalk.c` lists the CRC of every file on a volume,
for comparing a disk before and after on real hardware.

## Testing

Every test cuts the converter off after a chosen write, sometimes leaving that write
torn (half old, half new, or garbage), then runs it again: to roll back, or to finish.

- After a rollback, the whole image must equal the original block for block, except
  blocks the original FFS bitmap marks free.
- After a finished conversion, the PFS3 volume is read and every file's size and CRC
  must equal the FFS original, and the RDB must hold pfs3aio. On k8 and k8o some
  rolled-back images are also converted again and checked the same way.

### Version 0.2 (this code)

| test | build | write points | cases | failures |
|---|---|---|---|---|
| rollback after a power cut, and automatic rollback after a failed write | Amiga binary under vamos | every write of all 7 images, clean and torn | 85,536 rollbacks, 3,024 rollbacks that were themselves cut off or failed | 0 |
| rollback, as above | host build | every write of all 7 images, clean and torn | 85,536 rollbacks, 2,800 rollbacks that were themselves cut off or failed | 0 |
| finishing after a power cut | host build | every write of all 7 images, clean and torn | 42,768 resumes, 1,400 double crashes | 0 |
| finishing after a power cut | Amiga binary under vamos | 80 sampled writes of t4t and l8o, clean and torn | 320 resumes, 80 double crashes | 0 |

The vamos run took 7¼ hours with 12 tests running at once (`tests/run_vamos_all.sh`).
The host run took 26 minutes, also 12 at once (`tests/run_host_all.sh`).

The test images (made by `tools/`, see below):

| image | made by | volume | block writes in a conversion |
|---|---|---|---|
| t4t | `mktestimg.py tests/t4t.img 4 85 2` | 4 MB FFS, nearly full (867 blocks free) | 1,079 |
| l8f | `mklinkimg.py tests/l8f.img 8 23 ffs 88` | 8 MB FFS with hard and soft links | 1,283 |
| l8o | `mklinkimg.py tests/l8o.img 8 25 ofs 30` | 8 MB OFS with hard and soft links | 4,597 |
| h16 | `mktestimg2.py tests/h16.img 16 92 3` | 16 MB FFS, nested and big directories, comments, protection bits, fragmented files | 1,510 |
| o8 | `mktestimg2.py tests/o8.img 8 55 11 ofs` | 8 MB OFS, as h16 | 5,772 |
| k8 | `mkffs_handler.py tests/k8.img 8 2 31` | 8 MB FFS with 1 KB blocks, written by the real FFS 40.1 | 1,432 |
| k8o | `mkffs_handler.py tests/k8o.img 8 2 32 ofs 35` | 8 MB OFS with 1 KB blocks, written by the real FFS 40.1 | 5,711 |

The images are not in this repository: some hold Commodore's FastFileSystem in their
RDB. `mktestimg*.py` need xdftool (amitools); `mklinkimg.py` and `mkffs_handler.py`
need AmiFUSE and your own FastFileSystem 40.1 (Workbench 3.1 `L:FastFileSystem`) at
`ref/FastFileSystem_40.1`. The vamos tests need vamos (amitools).

### Real hardware (version 0.1, before rollback was added)

On an AA3000+ (an A3000 clone) with a 68060 card and a SCSI disk, two partitions were
converted, and the CRC of every file was taken before and after with `crcwalk`:

- Work: FFS with 1 KB blocks, 4,642 files and directories: all equal after.
- Diag: the boot volume, 2,867 files and 318 directories: all equal after, and it
  booted from the RDB's pfs3aio.

### Earlier builds (before rollback was added)

Finishing after a power cut, with the Python prototype and the first C version:

| build | volume (block writes in a conversion) | write points | torn writes too | failures |
|---|---|---|---|---|
| Python | t4 (590) | every write, 500 double crashes | yes, 3 kinds | 0 |
| Python | o8 (5,665) | every write, 300 double crashes | yes | 0 |
| Python | l8f (1,057) | every write, 300 double crashes | yes | 0 |
| Python | l8o (4,482) | 1,500 sampled, 200 double crashes | yes | 0 |
| Python | h16 (975) | 300 sampled, 100 double crashes | yes | 0 |
| Python | a copy of a CD32 hard disk (277,498) | 40 sampled, 10 double crashes | yes | 0 |
| Python | a copy of a Workbench disk (19,769) | 200 sampled, 50 double crashes | yes | 0 |
| C | t4t (992) | every write, 200 double crashes | yes, 3 kinds | 0 |
| C | k8 (1,373) | every write, 200 double crashes | yes, 2 kinds | 0 |
| C | k8o (5,704) | every write, 100 double crashes | no | 0 |

The converted CD32 and Workbench disks were then read with the real pfs3aio handler
(through AmiFUSE): all 11,177 and 3,172 files matched the FFS originals, and the
handler deleted some and wrote 300 new files on each with no errors.

## Limits

- The test suites use OFS and FFS (`DOS\0`, `DOS\1`). International and dircache
  volumes (`DOS\2` to `DOS\5`) are accepted but not in the suites.
- Blocks the FFS volume marked free may keep PFS3 data after a rollback. FFS never
  reads them.
- A torn write to block 1 can leave junk there. FFS ignores block 1 on hard disks.

## pfs3aio

`ref/pfs3aio` is pfs3aio 19.2 (the 3.1 release, 2.10.2018) by Michiel Pelt and Toni
Wilen, and the Amiga binary carries a copy of it. It is under the licence in
`ref/pfs3aio.LICENSE`: this product includes software developed by Michiel Pelt.
