#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for cdb (GPDB specific routines)
#
#-------------------------------------------------------------------------


subdir = src/backend/cdb
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
override CPPFLAGS := -I$(top_srcdir)/src/backend/gp_libpq_fe $(CPPFLAGS)

SUBDIRS := motion


OBJS = cdbanalyze.o \
       cdbappendonlystorage.o cdbappendonlystorageformat.o \
       cdbappendonlystorageread.o cdbappendonlystoragewrite.o \
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
	   cdbtargeteddispatch.o cdbthreadwork.o \
	   cdbtimer.o \
	   cdbtm.o cdbtmutils.o \
	   cdbutil.o \
	   cdbvars.o cdbvarblock.o \
	   cdbworkfile.o

ifeq ($(PORTNAME),cygwin)
.LIBPATTERNS := $(filter-out %.so,$(.LIBPATTERNS))
endif



ALLOBJS = $(OBJS) $(SUBDIROBJS)

include $(top_srcdir)/src/backend/common.mk
