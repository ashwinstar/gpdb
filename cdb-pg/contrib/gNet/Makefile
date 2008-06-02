 
subdir = contrib/gNet
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = gNet
OBJS       = bayes.o matrix.o pivot.o regress.o 

DATA_built = gNet.sql gNet_remove.sql

# Need to add lapack support for other platforms
ifeq ($(PORTNAME), darwin)
  DATA_built += pinv.o
  SHLIB_LINK += -lclapack
endif

include $(top_srcdir)/contrib/contrib-global.mk
