#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for cdb (GPDB specific routines)
#
#-------------------------------------------------------------------------


subdir = src/backend/postmaster/fts
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
override CPPFLAGS := -I$(top_srcdir)/src/backend/gp_libpq_fe $(CPPFLAGS)

OBJS = fts.o ftsprobe.o ftsfilerep.o ftssan.o
       
include $(top_srcdir)/src/backend/common.mk
