#
# Makefile for utils/stat
#
# $PostgreSQL: pgsql/src/backend/utils/stat/Makefile,v 1.60 2006/04/05 22:11:55 tgl Exp $
#

subdir = src/backend/utils/stat
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
override CPPFLAGS := -I$../../gp_libpq_fe  $(CPPFLAGS)

BLD_TOP = ../../../../..
include $(BLD_TOP)/Makefile.global
include $(BLD_TOP)/Makefile.thirdparty
CLAPACKDIR = $(BLD_TOP)/ext/$(BLD_ARCH)/lib
CLAPACKLIBS = -llapack -lf2c -lblas

ifeq "$(findstring osx,$(BLD_ARCH))" "osx"
LDOPTS = -Z
endif

OBJS = bayes.o pinv.o regress.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS) $(CLAPACKLIBS) $(LDOPTS) -L$(CLAPACKDIR)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif
