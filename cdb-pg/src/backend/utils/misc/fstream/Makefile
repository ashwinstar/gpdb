#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/misc/fstream
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/misc/fstream
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)

OBJS = fstream.o gfile.o

# This location might depend on the installation directories. Therefore
# we can't subsitute it into pg_config.h.
ifdef krb_srvtab
override CPPFLAGS += -DPG_KRB_SRVTAB='"$(krb_srvtab)"'
endif

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
