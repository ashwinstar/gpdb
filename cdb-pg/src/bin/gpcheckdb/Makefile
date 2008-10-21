#-------------------------------------------------------------------------
#
# Makefile for src/bin/gpcheckdb
#
#-------------------------------------------------------------------------

PGFILEDESC = "gpcheckdb - program to check consistency of GPDB databases"
subdir = src/bin/gpcheckdb
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -DFRONTEND -I$(libpq_srcdir) -g $(CPPFLAGS) 

CC=g++

OBJS= gpcheckdb.o

all: submake-libpq submake-libpgport gpcheckdb

gpcheckdb: $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(OBJS) $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)


install: all installdirs
	$(INSTALL_PROGRAM) gpcheckdb$(X) '$(DESTDIR)$(bindir)/lib/gpcheckdb$(X)'

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/lib/gpcheckdb$(X)' 

clean distclean:
	rm -f gpcheckdb$(X) $(OBJS)

maintainer-clean: distclean
