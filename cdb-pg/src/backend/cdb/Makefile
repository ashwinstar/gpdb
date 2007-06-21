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

OBJS = cdbanalyze.o \
	   cdbbackup.o \
	   cdbcat.o cdbconn.o cdbcopy.o \
	   cdbdisp.o cdbdispatchresult.o cdbdistributedxidmap.o cdbdistributedxacts.o cdbmaxdistributedxid.o cdbdistributedxid.o \
	   cdbexplain.o \
	   cdbfts.o \
	   cdbgang.o cdbgroup.o \
	   cdbhash.o cdbheap.o \
	   cdblink.o cdbllize.o cdblogsync.o \
	   cdbmutate.o \
	   cdbpath.o cdbpathlocus.o cdbpathtoplan.o cdbpgdatabase.o \
	   cdbplan.o cdbpullup.o \
	   cdbsetop.o cdbsrlz.o cdbsubplan.o cdbsubselect.o \
	   cdbtm.o \
	   cdbutil.o cdbrelsize.o \
	   cdbvars.o cdboidsync.o

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

ifeq ($(PORTNAME),freebsd)
	$(OBJCOPY) --localize-symbols=$(top_srcdir)/$(subdir)/libpq_local.sym SUBSYS.o
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
	egrep -v -f $(top_srcdir)/$(subdir)/mac_exclude_symbols $(top_srcdir)/$(subdir)/libpq_local.sym | sed -e '/^#/!s/^[[:space:]]*/_/' > libpq_local.sym.mac
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
