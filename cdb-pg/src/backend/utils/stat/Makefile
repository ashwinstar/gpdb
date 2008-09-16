#
# Makefile for utils/stat
#
# $PostgreSQL: pgsql/src/backend/utils/stat/Makefile,v 1.60 2006/04/05 22:11:55 tgl Exp $
#

subdir = src/backend/utils/stat
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS = bayes.o pinv.o regress.o


# Ugly gross hack.  Remove this
BLD_TOP = ../../../../..
include $(BLD_TOP)/Makefile.thirdparty
CLAPACKDIR = $(BLD_TOP)/ext/$(BLD_ARCH)/lib


CLAPACK = liblapack.a libblas.a libf2c.a 

all: SUBSYS.o

SUBSYS.o: $(OBJS) $(CLAPACK)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS) $(CLAPACK)

%.a:
	cp $(CLAPACKDIR)/$@ $@

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) $(CLAPACK)

ifeq (depend,$(wildcard depend))
include depend
endif
