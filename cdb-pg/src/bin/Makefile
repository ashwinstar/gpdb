#-------------------------------------------------------------------------
#
# Makefile for src/bin (client programs)
#
# Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/bin/Makefile,v 1.54 2009/01/01 17:23:53 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin
top_builddir = ../..
SUBDIRS = pg_dump pg_dump/cdb
include $(top_builddir)/src/Makefile.global

unittest-check:
	$(MAKE) -C gphdfs unittest
ifdef SUBDIRS
	@for dir in $(SUBDIRS); do if [ -d $$dir/test ]; then $(MAKE) -C $$dir/test check; fi; done
endif

DIRS = initdb ipcclean pg_ctl pg_dump \
	psql scripts pg_config pg_controldata pg_resetxlog \
	gpmapreduce gpmirrortransition gpupgrade gphdfs/src/protocol_formatter


ifeq ($(enable_gphdfs),yes)
DIRS+=gphdfs
endif

ifeq ($(enable_snmp),yes)
DIRS+=gpsnmpd
endif

ifeq ($(PORTNAME), win32)
DIRS+=pgevent
endif

all install installdirs uninstall distprep clean distclean maintainer-clean:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done
