#
# Makefile for utils/stat
#
# $PostgreSQL:  $
#

subdir = src/backend/utils/stat
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
override CPPFLAGS := -I$../../gp_libpq_fe  $(CPPFLAGS)

OBJS = bayes.o pinv.o regress.o

include $(top_srcdir)/src/backend/common.mk