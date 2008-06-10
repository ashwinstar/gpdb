PROGRAM = gp_filedump
OBJS	= gp_filedump.o pg_crc.o 

EXTRA_CLEAN = pg_crc.c 

DOCS = README.gp_filedump

ifdef USE_PGXS
PGXS := $(shell gp_config --gpxs)
include $(PGXS)
else
subdir = contrib/gp_filedump
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

pg_crc.c: $(top_srcdir)/src/backend/utils/hash/pg_crc.c
	rm -f $@ && $(LN_S) $< .
