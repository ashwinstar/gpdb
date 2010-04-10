MODULE_big = gp_svec
DATA = gp_svec.sql gp_sfv_test.sql
REGRESS = gp_svec_test.sql
OBJS = gp_sfv.o sparse_vector.o operators.o SparseData.o
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
#CC=gcc -no-cpp-precomp -m64
#CC=gcc -no-cpp-precomp -m32
