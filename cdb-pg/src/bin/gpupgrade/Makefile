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

OBJS= gpupgrade.o

all: submake-libpq gpupgrade

gpupgrade: gpupgrade.o $(libpq_builddir)/libpq.a 
	$(CC) $(CFLAGS) $(OBJS)  $(libpq_pgport) $(EXTRA_OBJS) $(LDFLAGS) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) gpupgrade$(X) '$(DESTDIR)$(bindir)'/gpupgrade$(X)

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f $(addprefix '$(DESTDIR)$(bindir)'/, gpupgrade$(X)

clean distclean maintainer-clean:
	rm -f gpupgrade$(X) $(OBJS) gpupgrade.o
