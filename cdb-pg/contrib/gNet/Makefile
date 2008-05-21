#include_dir := /Users/cwelton/greenplum-db-devel/include/postgresql/server

MODULE_big = gNet
OBJS       = bayes.o matrix.o pivot.o regress.o

DATA_built = gNet.sql gNet_remove.sql

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/gNet
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

