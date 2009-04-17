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

LIBS := $(filter-out -lreadline -ledit -ltermcap -lncurses -lcurses -lcurl -lssl -lz -lpam -lkrb5 -lgssapi_krb5 -lcom_err -lk5crypto -lnuma -lkrb5support, $(LIBS))

OBJS= gpmodcatversion.o pg_crc.o

UPGRADEFILES=$(top_srcdir)/src/test/regress/input/loadcat33.source.in \
	$(top_srcdir)/src/test/regress/data/upg2_pg_class_toadd.data.in \
	$(top_srcdir)/src/test/regress/data/upg2_pg_attribute_toadd.data.in \
	$(top_srcdir)/src/test/regress/data/upg2_pg_type_toadd.data.in \
	$(top_srcdir)/src/test/regress/data/upg2_pg_index_toadd.data.in \
	$(top_srcdir)/src/test/regress/data/upg2_pg_proc_toadd.data \
	$(top_srcdir)/src/test/regress/data/upg2_pg_namespace_toadd.data \
	$(top_srcdir)/src/test/regress/data/upg2_pg_description_toadd.data \
	$(top_srcdir)/src/test/regress/data/upgrade_conversion.sql.in \
	$(top_srcdir)/src/test/regress/data/downgrade_conversion.sql.in \
	$(top_srcdir)/src/test/regress/data/upg2_pg_depend_toadd.data

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
