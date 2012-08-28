PROGRAM = gp_clog_manipulator
OBJS	= gp_clog_operations.o gp_clog_manipulator_driver.o

#DOCS = README.gp_filedump

ifdef USE_PGXS
PGXS := $(shell gp_config --gpxs)
include $(PGXS)
else
subdir = contrib/gp_clog_manipulator
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
