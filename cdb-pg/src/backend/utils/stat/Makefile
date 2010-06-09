#
# Makefile for utils/stat
#
# $PostgreSQL: pgsql/src/backend/utils/stat/Makefile,v 1.60 2006/04/05 22:11:55 tgl Exp $
#

subdir = src/backend/utils/stat
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

BLD_TOP = ../../../../..
CLAPACKLIBS = -llapack -lf2c -lblas 
LIBS := $(filter-out -lesmtp -lnetsnmp -lpgport -lz -lcurl -lbz2 -lpam -lssl -lcrypto -lkrb5 -lcom_err -lz -ledit -lm -pthread -pthreads  , $(LIBS))


ifeq ($(PORTNAME), darwin)
LDOPTS = $(CLAPACKLIBS) -Z $(LIBS) 
endif

OBJS = bayes.o pinv.o regress.o

include $(top_srcdir)/src/backend/common.mk

