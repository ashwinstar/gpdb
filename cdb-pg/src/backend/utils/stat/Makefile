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
CLAPACKDIR = $(filter %lib,($(foreach dir,$(LDFLAGS), \
                $(if $(findstring cdb2/main/ext, $(dir)), \
                   $(subst -L,,$(dir))))))

CLAPACK = libf2c.a libblas.a liblapack.a

all: SUBSYS.o

SUBSYS.o: $(OBJS) $(CLAPACK)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS) $(CLAPACK)

%.a:
	cp /Users/swm/greenplum/p4/cdb2/main/ext/osx104_x86/lib/$@ $@

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif
