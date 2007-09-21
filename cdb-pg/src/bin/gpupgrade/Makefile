#-------------------------------------------------------------------------
#
# Makefile for src/bin/gpupgrade
#
# Portions Copyright (c) 2007, Greenplum
#
# $PostgreSQL$
#
#-------------------------------------------------------------------------

PGFILEDESC = "gpupgrade - Greenplum upgrader"
subdir = src/bin/gpupgrade
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -DFRONTEND -I$(libpq_srcdir) $(CPPFLAGS)

GPUOBJS= gpupgrade.o

GPCOBJS= gp_controldata_mod.o pg_crc.o $(WIN32RES)

all: submake-libpq gpupgrade gp_controldata_mod

gpupgrade: gpupgrade.o $(libpq_builddir)/libpq.a 
	$(CC) $(CFLAGS) $(GPUOBJS)  $(libpq_pgport) $(EXTRA_OBJS) $(LDFLAGS) $(LIBS) -o $@$(X)

gp_controldata_mod: gp_controldata_mod.o pg_crc.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LIBS) -o $@$(X)

pg_crc.c: $(top_srcdir)/src/backend/utils/hash/pg_crc.c
	rm -f $@ && $(LN_S) $< .

install: all installdirs
	$(INSTALL_PROGRAM) gpupgrade$(X) '$(DESTDIR)$(bindir)'/gpupgrade$(X)
	$(INSTALL_PROGRAM) gp_controldata_mod$(X) '$(DESTDIR)$(bindir)'/gp_controldata_mod$(X)

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)'/, gpupgrade$(X)
	rm -f '$(DESTDIR)$(bindir)'/, gp_controldata_mod$(X)

clean distclean maintainer-clean:
	rm -f gpupgrade$(X) $(OBJS) pg_crc.c
