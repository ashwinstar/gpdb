#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for executor
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/executor/Makefile,v 1.24 2006/08/02 01:59:45 joe Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/executor
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
OBJCOPY ?= objcopy

OBJS = execAmi.o execGrouping.o execHashagg.o execJunk.o execMain.o \
       execProcnode.o execQual.o execScan.o execTuples.o \
       execUtils.o functions.o instrument.o nodeAppend.o nodeAgg.o nodeWindow.o \
       nodeBitmapAnd.o nodeBitmapOr.o \
       nodeBitmapHeapscan.o nodeBitmapIndexscan.o nodeExternalscan.o nodeHash.o \
       nodeHashjoin.o nodeIndexscan.o nodeMaterial.o nodeMergejoin.o \
       nodeNestloop.o nodeAdaptiveNestloop.o nodeFunctionscan.o \
       nodeResult.o nodeSeqscan.o \
       nodeSetOp.o nodeShareInputScan.o \
       nodeSort.o nodeUnique.o nodeValuesscan.o nodeLimit.o nodeGroup.o \
       nodeSubplan.o nodeSubqueryscan.o nodeTidscan.o \
       nodeMotion.o tstoreReceiver.o spi.o

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
