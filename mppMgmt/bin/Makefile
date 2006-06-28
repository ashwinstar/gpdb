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


all: pylib

#
# Python Libraries
#
pylib : paramiko pycrypto

#
PARAMIKO_VERSION=1.5.4
PARAMIKO_DIR=paramiko-$(PARAMIKO_VERSION)
paramiko:
	@echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
	@echo "Unzipping Paramiko"
	@echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
	@cd $(PYLIB_SRC)/; unzip -o $(PARAMIKO_DIR).zip ; cd ../ ;
	@echo " Building paramiko "
	@cd $(PYLIB_SRC)/$(PARAMIKO_DIR) ; NO_M64=TRUE python setup.py build ; cd ../../
	cp -r $(PYLIB_SRC)/$(PARAMIKO_DIR)/build/lib/paramiko  $(LIB_DIR)/

paramikoInstall:
	@echo "Installing paramiko"

paramikoClean:
	@echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
	@echo "Cleaning Paramiko"
	@echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
	@cd $(PYLIB_SRC)/$(PARAMIKO_DIR) ; NO_M64=TRUE python setup.py clean; cd ../../
	@echo ""
	@echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
	@echo " Removing $(PARAMIKO_DIR) "
	@echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
	@echo ""
	@cd $(PYLIB_SRC)/; rm -rf $(PARAMIKO_DIR) ; cd ../

PYCRYPTO_VERSION=2.0.1
PYCRYPTO_DIR=pycrypto-$(PYCRYPTO_VERSION)

pycrypto:
	@echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
	@echo "Unzipping pycrypto"
	@echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
	@if [ `uname -s` = 'SunOS' ] ; then \
		cd $(PYLIB_SRC)/; gtar -xvzf $(PYCRYPTO_DIR).tar.gz ; cd ../ ;\
	else  \
		cd $(PYLIB_SRC)/; tar -xvzf $(PYCRYPTO_DIR).tar.gz ; cd ../ ;\
	fi
	@echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
	@echo " Building pycrypto "
	@echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
	@cd $(PYLIB_SRC)/$(PYCRYPTO_DIR) ; NO_M64=TRUE python setup.py build ; cd ../../
	@cp -r $(PYLIB_SRC)/$(PYCRYPTO_DIR)/build/lib.*/Crypto $(LIB_DIR)

pycryptoInstall:
	@echo "Installing pycrypto"

pycryptoClean:
	@echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
	@echo "Cleaning pycrypto"
	@echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
	@cd $(PYLIB_SRC)/$(PYCRYPTO_DIR) ; NO_M64=TRUE python setup.py clean; cd ../../
	@echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
	@echo " Removing $(PYCRYPTO_DIR) "
	@echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
	@cd $(PYLIB_SRC)/; rm -rf $(PYCRYPTO_DIR) ; cd ../

solarisTest:
	@if [ `uname -s` = 'SunOS' ]; then \
		echo "SOLARIS" ; \
	fi
clean : pycryptoClean paramikoClean

    
