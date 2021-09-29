EXTENSION = gp_snapshot
MODULES = gp_snapshot

EXTENSION_VERSION = 1.0


DATA = gp_snapshot--1.0.sql

OBJS = gp_snapshot.o

MODULE_big = gp_snapshot


ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = gpcontrib/gp_snapshot
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
