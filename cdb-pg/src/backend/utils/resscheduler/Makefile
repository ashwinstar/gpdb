#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/resscheduler
#
# IDENTIFICATION
#    $PostgreSQL$
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/resscheduler
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = resqueue.o resscheduler.o

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
