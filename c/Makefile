# host build (tests, in Docker: tftools-hosttest) and Amiga build (vbcc, in Docker: terriblefire78/spirom)
CORE = util.c rdb.c ffs.c plan.c pfs3.c convert.c
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -std=c99

f2p_host: $(CORE) host.c f2p.h
	$(CC) $(CFLAGS) -o $@ $(CORE) host.c

# Amiga: vbcc in terriblefire78/spirom; 68000 code runs on every Amiga
# pfs3aio is built in: ref/pfs3aio (19.2, the latest release, pfs3aio 3.1) as C data
ffs2pfs: $(CORE) amiga.c gui.c pfs3aio_bin.c f2p.h amiga.h
	vc +aos68k -c99 -cpu=68000 -O1 -I$(NDK32)/Include_H -o $@ $(CORE) amiga.c gui.c pfs3aio_bin.c -lamiga

pfs3aio_bin.c: ../ref/pfs3aio
	{ echo '/* ref/pfs3aio as C data: made by the Makefile, do not edit */'; \
	  echo 'const unsigned char pfs3aio_bin[] = {'; xxd -i < $<; echo '};'; \
	  echo 'const unsigned long pfs3aio_len = sizeof(pfs3aio_bin);'; } > $@

clean:
	rm -f f2p_host ffs2pfs pfs3aio_bin.c
