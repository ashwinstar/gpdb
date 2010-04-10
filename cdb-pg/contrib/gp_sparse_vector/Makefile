MODULE_big = gp_svec
DATA = gp_svec.sql gp_sfv_test.sql
REGRESS = gp_svec_test.sql
OBJS = gp_sfv.o sparse_vector.o operators.o SparseData.o
ifdef USE_PGXS
	PGXS := $(shell pg_config --pgxs)
	include $(PGXS)
else
	subdir = contrib/gp_sparse_vector
	top_builddir = ../..
	include $(top_builddir)/src/Makefile.global
	include $(top_srcdir)/contrib/contrib-global.mk
endif
#CC=gcc -no-cpp-precomp -m64
#CC=gcc -no-cpp-precomp -m32
