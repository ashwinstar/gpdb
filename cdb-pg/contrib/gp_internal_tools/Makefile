MODULE_big = gp_aoseg_history
OBJS       = gp_aoseg_history.o

PG_CPPFLAGS = -I$(libpq_srcdir)

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/gp_diagnostic_libraries
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
