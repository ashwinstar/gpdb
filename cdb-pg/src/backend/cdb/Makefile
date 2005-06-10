#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for cdb (CDB2)
#
# IDENTIFICATION
#    $Header$
#
#-------------------------------------------------------------------------


subdir = src/backend/cdb
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
OBJCOPY ?= objcopy

SUBDIRS := motion
SUBDIROBJS := $(SUBDIRS:%=%/SUBSYS.o)

OBJS = cdbvars.o cdbplan.o cdbllize.o cdbcat.o cdbutil.o cdblink.o cdbjoin.o \
	   cdbdisp.o cdbsrlz.o cdbhash.o cdbheap.o cdbagg.o cdbmutate.o \
	   cdbaggtactic.o cdbaggmutate.o cdbanalyze.o cdbcopy.o cdblimmutate.o \
	   cdbsubplan.o cdbtm.o cdbtmlog.o \
	   cdbpath.o cdbpathlocus.o

ifeq ($(PORTNAME),cygwin)
.LIBPATTERNS := $(filter-out %.so,$(.LIBPATTERNS))
endif

pqlibs := $(addprefix $(libpq_builddir)/,$(subst %,pq,$(.LIBPATTERNS)))

ifeq ($(PORTNAME),darwin)
pre_reqs := libpq_local.sym.mac
else
pre_reqs := $(top_srcdir)/$(subdir)/libpq_local.sym
endif

ALLOBJS = $(OBJS) $(SUBDIROBJS)

all: SUBSYS.o

SUBSYS.o: $(pqlibs) $(ALLOBJS) $(pre_reqs)
ifneq ($(PORTNAME),darwin)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(ALLOBJS) $(EXTRA_OBJS) $(libpq)
else
	# Force a static link.
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(ALLOBJS) $(EXTRA_OBJS) $(libpq_builddir)/libpq.a -unexported_symbols_list libpq_local.sym.mac
endif

ifeq ($(PORTNAME),linux)
	$(OBJCOPY) --localize-symbols=$(top_srcdir)/$(subdir)/libpq_local.sym SUBSYS.o
endif
ifeq ($(PORTNAME),solaris)
	$(OBJCOPY) --localize-symbols=$(top_srcdir)/$(subdir)/libpq_local.sym SUBSYS.o
endif

$(pqlibs) : submake-libpq

ifeq ($(PORTNAME),darwin)
libpq_local.sym.mac: $(top_srcdir)/$(subdir)/libpq_local.sym
	sed -e '/^#/!s/^[[:space:]]*/_/' $(top_srcdir)/$(subdir)/libpq_local.sym > libpq_local.sym.mac
endif	

$(SUBDIROBJS): $(SUBDIRS:%=%-recursive) ;

.PHONY: $(SUBDIRS:%=%-recursive)
$(SUBDIRS:%=%-recursive): 
	$(MAKE) -C $(subst -recursive,,$@) SUBSYS.o
        
depend dep:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@ || exit; done
	$(CC) -MM $(CFLAGS) *.c >depend

clean:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@ || exit; done
	rm -f SUBSYS.o $(OBJS) libpq_local.sym.mac

ifeq (depend,$(wildcard depend))
include depend
endif
