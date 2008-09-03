#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/external
#
#-------------------------------------------------------------------------

subdir = src/backend/access/external
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = fileam.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

fileam.o: fileam.c url.i

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif

