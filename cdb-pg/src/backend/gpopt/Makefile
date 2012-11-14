#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for gpopt
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/gpopt/Makefile,v 1.15 2008/02/19 10:30:07 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/gpopt
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

BLD_TOP = $(top_builddir)/..
include $(top_builddir)/../releng/releng.mk

include $(top_builddir)/src/backend/gpopt/gpopt.mk

SUBDIRS = config translate relcache utils

OBJS = CGPOptimizer.o gpdbwrappers.o

include $(top_srcdir)/src/backend/common.mk
