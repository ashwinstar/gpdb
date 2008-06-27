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

SUBDIRS := memprot
SUBDIROBJS := $(SUBDIRS:%=%/SUBSYS.o)

OBJS = bgwriter.o autovacuum.o service.o seqserver.o ddaserver.o walsendserver.o walredoserver.o pgarch.o pgstat.o postmaster.o syslogger.o \
	fork_process.o

ALLOBJS = $(OBJS) $(SUBDIROBJS)
all: SUBSYS.o

SUBSYS.o: $(ALLOBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(ALLOBJS)

$(SUBDIROBJS): $(SUBDIRS:%=%-recursive) ;

.PHONY: $(SUBDIRS:%=%-recursive)
$(SUBDIRS:%=%-recursive):
	$(MAKE) -C $(subst -recursive,,$@) SUBSYS.o

depend dep:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@ || exit; done
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@ || exit; done
	rm -f SUBSYS.o $(OBJS) 


ifeq (depend,$(wildcard depend))
include depend
endif
