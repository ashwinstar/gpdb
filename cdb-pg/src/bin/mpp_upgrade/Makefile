#-------------------------------------------------------------------------
#
# Makefile for src/bin/mpp_upgrade
#
#
#-------------------------------------------------------------------------

PGFILEDESC = "mpp_upgrade -- upgrades a Bizgres MPP 2.2 system to a 2.3 system"
subdir = src/bin/mpp_upgrade
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -DFRONTEND -DDEF_PGPORT=$(DEF_PGPORT) -I$(libpq_srcdir) $(CPPFLAGS)

OBJS=	mpp_upgrade.o

all: submake-libpq submake-libpgport mpp_upgrade

mpp_upgrade: $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(OBJS) $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) mpp_upgrade$(X) $(DESTDIR)$(bindir)/mpp_upgrade$(X)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/mpp_upgrade$(X)

clean distclean maintainer-clean:
	rm -f mpp_upgrade$(X) $(OBJS)


# ensure that changes in DEF_PGPORT propagate into object file
mpp_upgrade.o: mpp_upgrade.c $(top_builddir)/src/Makefile.global
