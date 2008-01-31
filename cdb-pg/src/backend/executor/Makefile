#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for executor
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/executor/Makefile,v 1.26 2007/06/11 01:16:22 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/executor
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
OBJCOPY ?= objcopy

OBJS = execAmi.o execGrouping.o execHashagg.o execJunk.o execMain.o \
       execProcnode.o execQual.o execScan.o execTuples.o \
       execUtils.o \
       functions.o \
       instrument.o \
       nodeAdaptiveNestloop.o nodeAppend.o nodeAgg.o \
       nodeBitmapAnd.o nodeBitmapOr.o \
       nodeBitmapHeapscan.o nodeBitmapIndexscan.o \
       nodeExternalscan.o \
       nodeFunctionscan.o \
       nodeGroup.o \
       nodeHash.o nodeHashjoin.o \
       nodeIndexscan.o \
       nodeLimit.o \
       nodeMaterial.o nodeMergejoin.o nodeMotion.o \
       nodeNestloop.o \
       nodeResult.o \
       nodeSeqscan.o nodeSetOp.o nodeShareInputScan.o nodeSort.o \
       nodeSubplan.o nodeSubqueryscan.o \
       nodeTidscan.o \
       nodeUnique.o \
       nodeValuesscan.o \
       nodeWindow.o \
       tstoreReceiver.o \
       spi.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean:
	rm -f SUBSYS.o $(OBJS)
