#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for motion (CDB2)
#
# IDENTIFICATION
#    $Header$
#
#-------------------------------------------------------------------------


subdir = src/backend/cdb/motion
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = cdbmotion.o

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
