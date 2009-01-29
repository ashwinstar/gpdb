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

all: gpmodcatversion

include $(top_srcdir)/src/Makefile.shlib  

gpmodcatversion: $(OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LIBS) -o $@$(X)

pg_crc.c: $(top_srcdir)/src/backend/utils/hash/pg_crc.c
	rm -f $@ && $(LN_S) $< .

install: all installdirs
	$(INSTALL_PROGRAM) gpmodcatversion$(X) \
		'$(DESTDIR)$(bindir)/lib'/gpmodcatversion$(X)

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)' '$(DESTDIR)$(bindir)/lib'

uninstall:
	rm -f '$(DESTDIR)$(bindir)'/lib, gpmodcatversion$(X)

clean distclean maintainer-clean: clean-lib
	rm -f gpmodcatversion$(X)
	rm -f pg_crc.c
