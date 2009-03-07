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
override CPPFLAGS := -I../gp_libpq_fe $(CPPFLAGS)

SUBDIRS := motion
SUBDIROBJS := $(SUBDIRS:%=%/SUBSYS.o)

OBJS = cdbanalyze.o \
	   cdbbackup.o cdbbufferedappend.o cdbbufferedread.o \
	   cdbcat.o cdbcellbuf.o cdbchunkbuf.o cdbchunkpool.o cdbconn.o cdbcopy.o \
	   cdbdisp.o cdbdispatchresult.o \
	   cdbdistributedsnapshot.o \
	   cdbdistributedxid.o cdbdistributedxacts.o \
	   cdbdtxcontextinfo.o \
	   cdbexplain.o \
	   cdbfts.o \
	   cdbgang.o cdbgroup.o \
	   cdbhash.o cdbheap.o \
	   cdblink.o cdbllize.o cdblocaldistribxact.o cdblogsync.o \
	   cdbmaxdistributedxid.o cdbmutate.o \
	   cdboidsync.o \
	   cdbpath.o cdbpathlocus.o cdbpathtoplan.o cdbpgdatabase.o \
	   cdbplan.o cdbpullup.o \
	   cdbrelsize.o \
	   cdbshareddoublylinked.o \
	   cdbsetop.o cdbsreh.o cdbsrlz.o cdbsubplan.o cdbsubselect.o \
	   cdbthreadwork.o \
	   cdbtimer.o \
	   cdbtm.o \
	   cdbutil.o \
	   cdbvars.o cdbvarblock.o \
	   cdbworkfile.o

ifeq ($(PORTNAME),cygwin)
.LIBPATTERNS := $(filter-out %.so,$(.LIBPATTERNS))
endif



ALLOBJS = $(OBJS) $(SUBDIROBJS)

all: SUBSYS.o

SUBSYS.o: $(ALLOBJS) $(pre_reqs)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(ALLOBJS) $(EXTRA_OBJS) 


$(SUBDIROBJS): $(SUBDIRS:%=%-recursive) ;

.PHONY: $(SUBDIRS:%=%-recursive)
$(SUBDIRS:%=%-recursive):
	$(MAKE) -C $(subst -recursive,,$@) SUBSYS.o

depend dep:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@ || exit; done
	$(CC) -MM $(CFLAGS) *.c >depend

clean:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@ || exit; done
	rm -f SUBSYS.o

ifeq (depend,$(wildcard depend))
include depend
endif
