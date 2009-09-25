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

OBJS= gpmodcatversion.o pg_crc.o

UPGRADEFILES := $(top_srcdir)/src/test/regress/input/loadcat33.source.in
UPGRADEFILES += $(top_srcdir)/src/test/regress/input/loadcat34.source.in 
UPGRADEFILES +=	$(wildcard $(top_srcdir)/src/test/regress/data/upgrade33/upg2*.data)
UPGRADEFILES +=	$(wildcard $(top_srcdir)/src/test/regress/data/upgrade33/upg2*.in)
UPGRADEFILES +=	$(wildcard $(top_srcdir)/src/test/regress/data/upgrade34/upg2*.data)
UPGRADEFILES +=	$(wildcard $(top_srcdir)/src/test/regress/data/upgrade34/upg2*.in)

all: gpmodcatversion

include $(top_srcdir)/src/Makefile.shlib  

gpmodcatversion: $(OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LIBS) -o $@$(X)

pg_crc.c: $(top_srcdir)/src/backend/utils/hash/pg_crc.c
	rm -f $@ && $(LN_S) $< .

install: all installdirs installupgradefiles
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

uninstall:
	rm -f '$(DESTDIR)$(bindir)'/lib, gpmodcatversion$(X)

clean distclean maintainer-clean: clean-lib
	rm -f gpmodcatversion$(X)
	rm -f gpmodcatversion.o
	rm -f pg_crc.o
	rm -f pg_crc.c
