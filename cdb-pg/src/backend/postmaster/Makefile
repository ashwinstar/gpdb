#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/postmaster
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/postmaster/Makefile,v 1.21 2005/07/14 05:13:40 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/postmaster
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
override CPPFLAGS := -I$(top_srcdir)/src/backend/gp_libpq_fe $(CPPFLAGS)

OBJS = bgwriter.o autovacuum.o service.o seqserver.o ddaserver.o walsendserver.o walredoserver.o pgarch.o pgstat.o postmaster.o syslogger.o \
	fork_process.o ftsprobe.o perfmon.o

ALLOBJS = $(OBJS) 
all: SUBSYS.o

SUBSYS.o: $(ALLOBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(ALLOBJS)

.PHONY: $(SUBDIRS:%=%-recursive)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 


ifeq (depend,$(wildcard depend))
include depend
endif
