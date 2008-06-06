 
subdir = contrib/gNet
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = gNet
OBJS       = bayes.o matrix.o pivot.o regress.o pinv.o pinv_test.o
DATA_built = gNet.sql gNet_remove.sql
#SHLIB_LINK += liblapack.a libblas.a libf2c.a
SHLIB_LINK += -llapack -lblas -lf2c

all: libf2c.a libblas.a liblapack.a

clean: lapack_clean

# We have a dependency on the lapack/blas libraries:
libf2c.a:
	@echo "--- Building libf2c ---"
	(cd clapack; $(MAKE) f2clib)
	cp clapack/F2CLIBS/libf2c.a libf2c.a

libblas.a:
	@echo "--- Building blas.a ---"
	(cd clapack; $(MAKE) blaslib)
	cp clapack/blas.a libblas.a

liblapack.a:
	@echo "--- Building clapack.a ---"
	(cd clapack; $(MAKE) lapacklib)
	cp clapack/lapack.a liblapack.a

lapack_clean:
	rm -f libf2c.a libblas.a liblapack.a
	(cd clapack; $(MAKE) clean)

include $(top_srcdir)/contrib/contrib-global.mk

