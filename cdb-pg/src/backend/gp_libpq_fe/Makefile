#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for gp_libpq_fe subsystem (internal frontend half of libpq to talk from MasterSeg to SegDBs)
#
# IDENTIFICATION
#
#-------------------------------------------------------------------------

subdir = src/backend/gp_libpq_fe
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


override CPPFLAGS :=  -DFRONTEND -DUNSAFE_STAT_OK -I$(srcdir) $(CPPFLAGS) -I$(top_builddir)/src/port
ifneq ($(PORTNAME), win32)
override CFLAGS += $(PTHREAD_CFLAGS)
endif

OBJS =  fe-exec.o fe-secure.o  pqexpbuffer.o thread.o \
    fe-auth.o   \
    fe-connect.o fe-misc.o fe-protocol3.o


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
