#
# Makefile for utils/stat
#
# $PostgreSQL: pgsql/src/backend/utils/stat/Makefile,v 1.60 2006/04/05 22:11:55 tgl Exp $
#

subdir = src/backend/utils/stat
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# This Makefile is different from other backend Makefiles.  This is necessary
# only on OSX (darwin), and it is to solve a specific issue.
#
# On OSX, there are versions of liblapack and libblas already installed in /usr/lib,
# and the ld command will prefer those over the ones we build and put in the ext directory.
# These versions are incompatible with ours, and compiling with our headers and linking to them will
# result in a SIGSEGV.
#
# So, we statically link these libraries at this time.  This is unfortunate as it forces us to use the 
# old style SUBSYS,o partial linking, but it solves the problem.
#

# CLAPACKLIBS is probably not needed, as those should already be in LIBS, but it can't hurt.
CLAPACKLIBS = -llapack -lf2c -lblas 

# We need to filter out everything we don't want linked in right now so that we can extract the 
# location of our libraries (for example, ./cdb2/main/ext/osx106_x86/lib).
LIBS := $(filter-out -lesmtp -lnetsnmp -lpgport -lz -lcurl -lbz2 -lpam -lssl -lcrypto -lkrb5 -lcom_err -lz -ledit -lm -pthread -pthreads  , $(LIBS))

# The -Z option is critical, as it tells the OSX LD program not to automatically search the default
# locations (/usr/lib, for example).  the ld program on other platforms don't have a -Z option, so will
# fail if we pass in this option.
ifeq ($(PORTNAME), darwin)
LDOPTS = $(CLAPACKLIBS) -Z $(LIBS) 
endif

OBJS = bayes.o pinv.o regress.o student.o

include $(top_srcdir)/src/backend/common.mk

