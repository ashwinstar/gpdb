 
subdir = contrib/gNet
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = gNet
OBJS       = bayes.o matrix.o pivot.o regress.o pinv.o
DATA_built = gNet.sql gNet_remove.sql
SHLIB_LINK += lapack.a blas.a libf2c.a

all: libf2c.a blas.a lapack.a

clean: lapack_clean

# We have a dependency on the lapack/blas libraries:
libf2c.a:
	@echo "--- Building libf2c ---"
	(cd clapack; $(MAKE) f2clib)
	ln -s clapack/F2CLIBS/libf2c.a .

blas.a:
	@echo "--- Building blas.a ---"
	(cd clapack; $(MAKE) blaslib)
	ln -s clapack/blas.a

lapack.a:
	@echo "--- Building clapack.a ---"
	(cd clapack; $(MAKE) lapacklib)
	ln -s clapack/lapack.a .

lapack_clean:
	rm -f libf2c.a blas.a lapack.a
	(cd clapack; $(MAKE) clean)

include $(top_srcdir)/contrib/contrib-global.mk

