################################################################################ 
# Makefile  Makefile for Python Modules and other utilities
# Copyright Greenplum 2006, 2007
################################################################################

# protect the default target for this file from the targets in Makefile.global
# and Makefile.thirdparty
default: all

BLD_TOP=../..
include $(BLD_TOP)/Makefile.global
include $(BLD_TOP)/Makefile.thirdparty

#
# SOURCE DIRECTORIES
#
SRC=$(CURDIR)
PYLIB_SRC=$(SRC)/pythonSrc


#
# INSTALL DIRECTORY
#
LIB_DIR=$(SRC)/lib
PYLIB_DIR=$(SRC)/ext

all: paramiko pycrypto pygresql stream netperf

#
# Python Libraries
#


NETPERF_DIR=netperf-2.4.3
netperf:
	@echo "--- netperf"
	if [ ! -f $(SRC)/src/$(NETPERF_DIR)/Makefile ]; then \
		(cd $(SRC)/src && gzip -d -c $(NETPERF_DIR).tar.gz | tar xvf - && cd $(NETPERF_DIR) && ./configure) \
	fi
	cd $(SRC)/src/$(NETPERF_DIR) && $(MAKE)
	cp $(SRC)/src/$(NETPERF_DIR)/src/netperf $(SRC)/src/$(NETPERF_DIR)/src/netserver lib/

#
#  STREAM
#
STREAM_DIR=$(SRC)/src/stream
stream:
	@echo "--- stream"
	@cd $(STREAM_DIR) ; NO_M64=TRUE gcc  stream.c -o stream; cd ../../
	@cp $(STREAM_DIR)/stream lib/stream 

#
# PyGreSQL
#
PYGRESQL_VERSION=4.0
PYGRESQL_DIR=PyGreSQL-$(PYGRESQL_VERSION)
pygresql:
	@echo "--- PyGreSQL"
	cd $(PYLIB_SRC)/ && $(TAR) xzf $(PYGRESQL_DIR).tar.gz
	cd $(INSTLOC)/ && . greenplum_path.sh && unset PYTHONHOME \
	  && cd $(PYLIB_SRC)/$(PYGRESQL_DIR) && CC="$(strip gcc $(BLD_CFLAGS))" LDFLAGS="$(BLD_CFLAGS)" python setup.py build
	mkdir -p $(PYLIB_DIR)/pygresql
	cp -r $(PYLIB_SRC)/$(PYGRESQL_DIR)/build/lib.*/* $(PYLIB_DIR)/pygresql
	touch $(PYLIB_DIR)/__init__.py

#
# PARAMIKO
#
PARAMIKO_VERSION=1.6.4.1
PARAMIKO_DIR=paramiko-$(PARAMIKO_VERSION)
paramiko:
	@echo "--- paramiko"
	cd $(PYLIB_SRC)/ && $(TAR) xzf $(PARAMIKO_DIR).tgz
	cd $(PYLIB_SRC)/$(PARAMIKO_DIR)/ && python setup.py build
	cp -r $(PYLIB_SRC)/$(PARAMIKO_DIR)/build/lib/paramiko  $(PYLIB_DIR)/


#
# PYCRYPTO
#
PYCRYPTO_VERSION=2.0.1
PYCRYPTO_DIR=pycrypto-$(PYCRYPTO_VERSION)

pycrypto:
	@echo "--- pycrypto"
	cd $(PYLIB_SRC)/ && $(TAR) xzf $(PYCRYPTO_DIR).tar.gz
	cd $(PYLIB_SRC)/$(PYCRYPTO_DIR)/ && CC="$(strip gcc $(BLD_CFLAGS))" LDFLAGS="$(BLD_CFLAGS)" python setup.py build
	cp -r $(PYLIB_SRC)/$(PYCRYPTO_DIR)/build/lib.*/Crypto $(PYLIB_DIR)

solarisTest:
	@if [ `uname -s` = 'SunOS' ]; then \
		echo "SOLARIS" ; \
	fi

clean :
	@rm -rf $(PYLIB_SRC)/$(PARAMIKO_DIR)
	@rm -rf $(PYLIB_SRC)/$(PYCRYPTO_DIR)
	@rm -rf $(PYLIB_SRC)/$(PYGRESQL_DIR)
	@rm -rf $(STREAM_DIR)/stream lib/stream  
	@if [ -f $(SRC)/src/$(NETPERF_DIR)/Makefile ]; then \
		(cd $(SRC)/src/$(NETPERF_DIR) && $(MAKE) clean) \
	fi
	@rm -rf *.pyc lib/*.pyc

distclean: clean
	@rm -rf $(NETPERF_DIR)    
