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

# The frontend doesn't need everything that's in LIBS, some are backend only
LIBS := $(filter-out -llapack -lblas -lf2c -lresolv -lbz2, $(LIBS))
LIBS := $(filter-out -lreadline -ledit -ltermcap -lncurses -lcurses -lcurl -lssl -lz -lpam -lkrb5 -lgssapi_krb5 -lcom_err -lk5crypto -lnuma -lkrb5support, $(LIBS))

MODCAT_OBJS= gpmodcatversion.o pg_crc.o

UPGRADEFILES := $(top_srcdir)/src/test/regress/input/loadcat33.source.in
UPGRADEFILES += $(top_srcdir)/src/test/regress/input/loadcat34.source.in 
UPGRADEFILES += $(top_srcdir)/src/test/regress/input/loadcat34_sh.source.in 
UPGRADEFILES +=	$(wildcard $(top_srcdir)/src/test/regress/data/upgrade33/upg2*.data)
UPGRADEFILES +=	$(wildcard $(top_srcdir)/src/test/regress/data/upgrade33/upg2*.in)
UPGRADEFILES +=	$(wildcard $(top_srcdir)/src/test/regress/data/upgrade34/upg2*.data)
UPGRADEFILES +=	$(wildcard $(top_srcdir)/src/test/regress/data/upgrade34/upg2*.in)

# This sets things up to create a "contrib" module for upgrade extensions
MODULE_big = gpupgrade
OBJS= gpviewcp.o
include $(top_srcdir)/contrib/contrib-global.mk

# In addition to the contrib module we are also building the gpmodcatversion program
all: gpmodcatversion

clean distclean maintainer-clean: clean-lib gpmodcat_clean

install: all installdirs installupgradefiles gpmodcat_install

uninstall: gpmodcat_uninstall

# The rest is to build the gpmodcatversion program
gpmodcatversion: $(MODCAT_OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LIBS) -o $@$(X)

pg_crc.c: $(top_srcdir)/src/backend/utils/hash/pg_crc.c
	rm -f $@ && $(LN_S) $< .

gpmodcat_clean:
	rm -f gpmodcatversion$(X)
	rm -f $(MODCAT_OBJS)
	rm -f pg_crc.c

gpmodcat_install: gpmodcatversion
	$(INSTALL_PROGRAM) gpmodcatversion$(X) \
		'$(DESTDIR)$(bindir)/lib'/gpmodcatversion$(X)

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)' '$(DESTDIR)$(bindir)/lib'
	$(mkinstalldirs) '$(DESTDIR)$(datadir)' '$(DESTDIR)$(datadir)/upgrade'

installupgradefiles: $(UPGRADEFILES)
	@for i in $(UPGRADEFILES); \
	do \
		$(INSTALL_DATA) $$i '$(DESTDIR)$(datadir)/upgrade/'`basename $$i`; \
	done

gpmodcat_unistall:
	rm -f '$(DESTDIR)$(bindir)'/lib, gpmodcatversion$(X)

