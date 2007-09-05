################################################################################ 
# Makefile  Makefile for Python Modules
# Author   : Koneru Srikanth
# Company  : GreenPlum.com
# Date     : Jan 30th 2006
################################################################################

#
# SOURCE DIRECTORIES
#
SRC=`pwd`
PYLIB_SRC=$(SRC)/pythonSrc


#
# INSTALL DIRECTORY
#
LIB_DIR=$(SRC)/lib


all: pylib stream netperf

#
# Python Libraries
#
pylib : paramiko pycrypto 


NETPERF_DIR=netperf-2.4.3
netperf:
	@echo "--- netperf"
	@if [ ! -f $(SRC)/src/$(NETPERF_DIR)/Makefile ]; then \
		(cd $(SRC)/src && gzip -d -c $(NETPERF_DIR).tar.gz | tar xvf - && cd $(NETPERF_DIR) && ./configure) \
	fi
	@(cd $(SRC)/src/$(NETPERF_DIR) && $(MAKE))
	cp $(SRC)/src/$(NETPERF_DIR)/src/netperf $(SRC)/src/$(NETPERF_DIR)/src/netserver lib/

netperfClean:
	@echo "--- Cleaning netperf utility."
	@if [ -f $(SRC)/src/$(NETPERF_DIR)/Makefile ]; then \
		(cd $(SRC)/src/$(NETPERF_DIR) && make clean) \
	fi

#
#  STREAM
#
STREAM_DIR=$(SRC)/src/stream
stream:
	@echo "--- stream"
	@cd $(STREAM_DIR) ; NO_M64=TRUE gcc  stream.c -o stream; cd ../../
	@cp $(STREAM_DIR)/stream lib/stream 

streamClean:
	@echo "--- cleaning stream"
	@rm -rf $(STREAM_DIR)/stream lib/stream > /dev/null 



stream:

#
# PARAMIKO
#
PARAMIKO_VERSION=1.6.4.1
PARAMIKO_DIR=paramiko-$(PARAMIKO_VERSION)
paramiko:
	@echo "--- paramiko"
	@if [ `uname -s` = 'SunOS' ] ; then \
		(cd $(PYLIB_SRC)/ && gtar -xzf $(PARAMIKO_DIR).tgz)\
	else  \
		(cd $(PYLIB_SRC)/ && tar -xzf $(PARAMIKO_DIR).tgz);\
	fi
	@(cd $(PYLIB_SRC)/$(PARAMIKO_DIR) && NO_M64=TRUE python setup.py build)
	@cp -r $(PYLIB_SRC)/$(PARAMIKO_DIR)/build/lib/paramiko  $(LIB_DIR)/

paramikoClean:
	@echo "--- cleaning paramiko"
	@rm -rf $(PYLIB_SRC)/$(PARAMIKO_DIR)


#
# PYCRYPTO
#
PYCRYPTO_VERSION=2.0.1
PYCRYPTO_DIR=pycrypto-$(PYCRYPTO_VERSION)

pycrypto:
	@echo "--- pycrypto"
	@if [ `uname -s` = 'SunOS' ] ; then \
		(cd $(PYLIB_SRC)/ && gtar -xzf $(PYCRYPTO_DIR).tar.gz)\
	else  \
		(cd $(PYLIB_SRC)/; tar -xzf $(PYCRYPTO_DIR).tar.gz)\
	fi
	@(cd $(PYLIB_SRC)/$(PYCRYPTO_DIR) && NO_M64=TRUE python setup.py build)
	@cp -r $(PYLIB_SRC)/$(PYCRYPTO_DIR)/build/lib.*/Crypto $(LIB_DIR)


pycryptoClean:
	@echo "--- cleaning pycrypto"
	@rm -rf $(PYLIB_SRC)/$(PYCRYPTO_DIR)

solarisTest:
	@if [ `uname -s` = 'SunOS' ]; then \
		echo "SOLARIS" ; \
	fi

clean : pycryptoClean paramikoClean 
	@rm -rf *.pyc lib/*.pyc

    
