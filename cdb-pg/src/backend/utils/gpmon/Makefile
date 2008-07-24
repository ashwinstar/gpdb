#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/fmgr
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/fmgr/Makefile,v 1.17 2004/05/21 20:56:49 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/gpmon
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = gpmon.o


all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif
