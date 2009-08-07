#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/appendonly
#
#-------------------------------------------------------------------------

subdir = src/backend/access/appendonly
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = appendonlyam.o aosegfiles.o aomd.o appendonlywriter.o

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

