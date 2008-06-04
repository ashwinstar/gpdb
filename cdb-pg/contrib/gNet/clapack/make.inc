####################################################################
#  CLAPACK make include file.                                       #
#  CLAPACK, Version 3.1.1                                           #
#  February 2008                                                    #
####################################################################
CC        = gcc

# if no wrapping of the blas library is needed, uncomment next line
#CC        = gcc -DNO_BLAS_WRAP
CFLAGS    = -O3
LOADER    = gcc
LOADOPTS  = 
NOOPT     = -O0 
DRVCFLAGS = $(CFLAGS)
F2CCFLAGS = $(CFLAGS)

#######################################################################
#
#  The archiver and the flag(s) to use when building archive (library)
#  If you system has no ranlib, set RANLIB = echo.
#
ARCH     = ar
ARCHFLAGS= cr
RANLIB   = ranlib
#
#  The location of the libraries to which you will link.  (The 
#  machine-specific, optimized BLAS library should be used whenever
#  possible.)
#

# Using reference BLAS
BLASLIB      = ../../blas.a

# Using Vectib
#BLASLIB = -framework vecLib
# Using Fortran BLAS interface of ATLAS without wrapper (CLAPACK has to be compiled with -DNO_BLAS_WRAP)
#BLASLIB = -lf77blas -latlas
# Using Fortran BLAS interface of ATLAS with wrapper
#BLASLIB = ../../libfblaswr.a -lf77blas -latlas
# Using CBLAS interface of ATLAS with wrapper
#BLASLIB = ../../libcblaswr.a -lcblas -latlas
# Using Goto without wrapper (CLAPACK has to be compiled with -DNO_BLAS_WRAP)
#BLASLIB = -lgoto 
# Using Goto with wrapper
#BLASLIB = ../../libfblaswr.a -lgoto 

LAPACKLIB    = lapack.a
F2CLIB       = ../../F2CLIBS/libf2c.a
TMGLIB       = tmglib.a
EIGSRCLIB    = eigsrc.a
LINSRCLIB    = linsrc.a

