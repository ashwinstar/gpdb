all: gppkg

BLD_TOP=../../../../../
GPMGMT=$(BLD_TOP)/gpMgmt
include $(BLD_TOP)/releng/tools.mk
include $(BLD_TOP)/releng/releng.mk
include $(BLD_TOP)/Makefile.global

OS=$(word 1,$(subst _, ,$(BLD_ARCH)))
ARCH=$(shell uname -p)

PERL_VER=5.12.4
PERL_REL=1
PERL_RPM_FLAGS="--define 'perl_dir $(PERL_DIR)' --define 'perl_ver $(PERL_VER)' --define 'perl_rel $(PERL_REL)'"
PERL_RPM=perl-$(PERL_VER)-$(PERL_REL).$(ARCH).rpm

PLPERL_VER=1.0
PLPERL_REL=1
PLPERL_DIR=`cd .. && pwd`
PLPERL_RPM_FLAGS="--define 'plperl_dir $(PLPERL_DIR)' --define 'plperl_ver $(PLPERL_VER)' --define 'plperl_rel $(PLPERL_REL)' --define 'perl_ver $(PERL_VER)'"
PLPERL_RPM=plperl-$(PLPERL_VER)-$(PLPERL_REL).$(ARCH).rpm
PLPERL_GPPKG=plperl-$(PLPERL_VER)-$(OS)-$(ARCH).gppkg

TARGET_GPPKG=$(PLPERL_GPPKG)
EXTRA_CLEAN+=$(PERL_RPM) $(PLPERL_RPM) $(PLPERL_GPPKG)

#
# Generic rules to build gppkgs included here
#
include $(BLD_TOP)/releng/gppkg.mk

gppkg: 
	echo $(PERL_DIR)
	$(MAKE) $(PERL_RPM) RPM_FLAGS=$(PERL_RPM_FLAGS)
	$(MAKE) $(PLPERL_RPM) RPM_FLAGS=$(PLPERL_RPM_FLAGS)
	$(MAKE) $(PLPERL_GPPKG) MAIN_RPM=$(PLPERL_RPM) DEPENDENT_RPMS=$(PERL_RPM)

.PHONY: gppkg
