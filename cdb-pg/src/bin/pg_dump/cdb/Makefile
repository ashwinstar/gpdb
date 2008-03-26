#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_dump/cdb
#
# Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $Header: /var/cvsroot/cdb/cdb-pg/src/bin/pg_dump/cdb/Makefile,v 1.2 2004/08/06 23:25:50 donniep Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pg_dump/cdb
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS= ../pg_backup_db.o ../pg_backup_custom.o \
	../pg_backup_files.o ../pg_backup_null.o ../pg_backup_tar.o \
	../dumputils.o

EXTRA_OBJS = $(top_builddir)/src/backend/parser/keywords.o

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS) -DBINDIR=\"$(bindir)\"

all: submake-libpq submake-libpgport submake-backend cdb_dump cdb_dump_agent cdb_restore cdb_restore_agent

cdb_dump: cdb_dump.o cdb_backup_status.o cdb_seginst.o cdb_backup_state.o cdb_table.o cdb_dump_util.o $(libpq_builddir)/libpq.a 
	$(CC) $(CFLAGS) cdb_dump.o cdb_backup_status.o cdb_seginst.o cdb_backup_state.o cdb_table.o cdb_dump_util.o $(libpq) $(LDFLAGS) $(LIBS) -o $@

cdb_dump_agent: cdb_dump_agent.o ../common.o ../pg_dump_sort.o cdb_backup_archiver.o cdb_dump_util.o cdb_seginst.o cdb_table.o cdb_backup_status.o $(OBJS) $(libpq_builddir)/libpq.a 
	$(CC) $(CFLAGS) cdb_dump_agent.o ../common.o ../pg_dump_sort.o cdb_dump_util.o cdb_seginst.o cdb_table.o cdb_backup_archiver.o cdb_backup_status.o $(OBJS) $(EXTRA_OBJS) $(libpq) $(LDFLAGS) $(LIBS) -o $@

#cdb_dumpall_agent: cdb_dumpall_agent.o ../common.o cdb_backup_archiver.o cdb_dump_util.o cdb_seginst.o cdb_table.o cdb_backup_status.o $(OBJS) $(libpq_builddir)/libpq.a 
#	$(CC) $(CFLAGS) cdb_dumpall_agent.o cdb_backup_archiver.o $(OBJS) $(EXTRA_OBJS) $(libpq) $(LDFLAGS) $(LIBS) -o $@

cdb_restore: cdb_restore.o cdb_backup_status.o cdb_seginst.o cdb_backup_state.o cdb_table.o cdb_dump_util.o $(libpq_builddir)/libpq.a 
	$(CC) $(CFLAGS) cdb_restore.o cdb_backup_status.o cdb_seginst.o cdb_backup_state.o cdb_table.o cdb_dump_util.o $(libpq) $(LDFLAGS) $(LIBS) -o $@
	
cdb_restore_agent: cdb_restore_agent.o cdb_backup_archiver.o cdb_backup_status.o cdb_dump_util.o cdb_seginst.o cdb_table.o $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) cdb_restore_agent.o  cdb_backup_archiver.o  cdb_backup_status.o cdb_dump_util.o cdb_seginst.o cdb_table.o $(OBJS) $(EXTRA_OBJS) $(libpq) $(LDFLAGS) $(LIBS) -o $@
	
.PHONY: submake-backend
submake-backend:
	$(MAKE) -C $(top_builddir)/src/backend/parser keywords.o
	
install: all installdirs
	$(INSTALL_PROGRAM) cdb_dump$(X) $(DESTDIR)$(bindir)/gp_dump$(X)
	$(INSTALL_PROGRAM) cdb_restore$(X) $(DESTDIR)$(bindir)/gp_restore$(X)
	$(INSTALL_PROGRAM) cdb_dump_agent$(X) $(DESTDIR)$(bindir)/gp_dump_agent$(X)
	$(INSTALL_PROGRAM) cdb_restore_agent$(X) $(DESTDIR)$(bindir)/gp_restore_agent$(X)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(bindir)/, cdb_dump$(X) cdb_dump_agent$(X) cdb_restore$(X) cdb_restore_agent$(X))  

clean distclean maintainer-clean:
	rm -f cdb_dump$(X) cdb_restore$(X) cdb_dump_agent$(X) cdb_restore_agent$(X) cdbheadsync$(X) cdbheadmakeactive$(X) $(OBJS) $(OBJS) cdb_dump.o cdb_restore.o cdb_dump_agent.o cdb_restore_agent.o ../common.o cdb_backup_status.o \
	cdb_seginst.o cdb_backup_state.o cdb_table.o cdb_dump_util.o cdb_backup_archiver.o cdbheadsync.o cdbheadmakeactive.o  
