#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for motion (CDB2)
#
# IDENTIFICATION
#    $Header$
#
#-------------------------------------------------------------------------


subdir = src/backend/cdb/motion
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(top_srcdir)/src/backend/gp_libpq_fe $(CPPFLAGS)

OBJS = cdbmotion.o tupchunklist.o tupser.o  \
	ic_common.o ic_tcp.o ic_udp.o htupfifo.o

include $(top_srcdir)/src/backend/common.mk
