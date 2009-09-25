#-------------------------------------------------------------------------
#
# Makefile for src/bin/gpcheckdb
#
#-------------------------------------------------------------------------

PGFILEDESC = "gpcheckdb - program to check consistency of GPDB databases"
subdir = src/bin/gpcheckdb
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -DFRONTEND -I$(libpq_srcdir) -Wno-write-strings -g  $(CPPFLAGS) 

# The frontend doesn't need everything that's in LIBS, some are backend only
LIBS := $(filter-out -llapack -lblas -lf2c -lresolv -lbz2, $(LIBS))
# This program isn't interactive, so doesn't need these
LIBS := $(filter-out -lreadline -ledit -ltermcap -lncurses -lcurses -lcurl -lssl -lcrypto -lz, $(LIBS))

CXX += $(strip $(findstring -m64,$(CC)) $(findstring -m32,$(CC)))

OBJS= gpcheckdb.o

all: submake-libpq submake-libpgport gpcheckdb

gpcheckdb: $(OBJS) $(libpq_builddir)/libpq.a 
	$(CXX) $(CFLAGS) $(OBJS) $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)


install: all installdirs
	$(INSTALL_PROGRAM) gpcheckdb$(X) '$(DESTDIR)$(bindir)/lib/gpcheckdb$(X)'

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)'

uninstall:ok
	rm -f '$(DESTDIR)$(bindir)/lib/gpcheckdb$(X)' 

clean distclean:
	rm -f gpcheckdb$(X) $(OBJS)

maintainer-clean: distclean
