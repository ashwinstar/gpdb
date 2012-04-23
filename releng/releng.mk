##-------------------------------------------------------------------------------------
##
## Copyright (C) 2011 EMC - Data Computing Division (DCD)
##
## @doc: Engineering Services makefile utilities
##
## @author: eespino
##
##-------------------------------------------------------------------------------------

.PHONY: opt_write_test sync_tools clean_tools

#-------------------------------------------------------------------------------------
# machine and OS properties
#-------------------------------------------------------------------------------------

UNAME = $(shell uname)
UNAME_P = $(shell uname -p)

UNAME_ALL = $(UNAME).$(UNAME_P)

ARCH_FLAGS = -m$(BLD_BITS)

# shared lib support
ifeq (Darwin, $(UNAME))
	LDSFX = dylib
else
	LDSFX = so
endif

##-------------------------------------------------------------------------------------
## dependent modules
##
## NOTE: Dependent project module version is kept in $(BLD_TOP)/releng/make/dependencies/ivy.xml
##-------------------------------------------------------------------------------------

ifeq "$(BLD_ARCH)" "osx105_x86"
EXTRA_EXT=_32
endif

BLD_TYPE=debug

OBJDIR_DEFAULT = .obj.$(UNAME_ALL)$(ARCH_FLAGS).$(BLD_TYPE)

GREP_SED_VAR = $(BLD_TOP)/releng/make/dependencies/ivy.xml | sed -e 's|\(.*\)rev="\(.*\)"[ ]*conf\(.*\)|\2|'

XERCES_VER  = $(shell grep "\"xerces-c\""   $(GREP_SED_VAR))
LIBGPOS_VER = $(shell grep "\"libgpos\""    $(GREP_SED_VAR))
OPTIMIZER_VER = $(shell grep "\"optimizer\""  $(GREP_SED_VAR))

XERCES =  /opt/releng/tools/xerces/xerces-c/$(XERCES_VER)/$(BLD_ARCH)$(EXTRA_EXT)
XERCES_LIBDIR = $(XERCES)/lib

LIBGPOS = /opt/releng/tools/emc/libgpos/$(LIBGPOS_VER)/$(BLD_ARCH)$(EXTRA_EXT)/libgpos
LIBGPOS_LIBDIR = $(LIBGPOS)/$(OBJDIR_DEFAULT)

OPTIMIZER = /opt/releng/tools/emc/optimizer/$(OPTIMIZER_VER)/$(BLD_ARCH)$(EXTRA_EXT)
LIBGPOPT_LIBDIR = $(OPTIMIZER)//libgpopt/$(OBJDIR_DEFAULT)
LIBGPOPTUDF_LIBDIR = $(OPTIMIZER)/libgpoptudf/$(OBJDIR_DEFAULT)
LIBNAUCRATES_LIBDIR = $(OPTIMIZER)/libnaucrates/$(OBJDIR_DEFAULT)

## ---------------------------------------
## R-Project support
## ---------------------------------------

R_VER = $(shell grep 'name="R"' $(GREP_SED_VAR))

ifneq "$(wildcard /opt/releng/tools/R-Project/R/$(R_VER)/$(BLD_ARCH)/lib64)" ""
R_HOME = /opt/releng/tools/R-Project/R/$(R_VER)/$(BLD_ARCH)/lib64/R
else
ifneq "$(wildcard /opt/releng/tools/R-Project/R/$(R_VER)/$(BLD_ARCH)/lib)" ""
R_HOME = /opt/releng/tools/R-Project/R/$(R_VER)/$(BLD_ARCH)/lib/R
endif
endif

GPERF_VERSION = $(shell grep 'name="gperf"' $(GREP_SED_VAR))
ifneq "$(wildcard /opt/releng/tools/gnu/gperf/$(GPERF_VERSION)/$(BLD_ARCH)/bin/gperf)" ""
GPERF_DIR = /opt/releng/tools/gnu/gperf/$(GPERF_VERSION)/$(BLD_ARCH)
gperftmpPATH:=$(GPERF_DIR)/bin:$(PATH)
export PATH=$(gperftmpPATH)
endif

display_dependent_vers:
	@echo ""
	@echo "======================================================================"
	@echo " R_HOME ........ : $(R_HOME)"
	@echo " R_VER ......... : $(R_VER)"
	@echo " CONFIGFLAGS ... : $(CONFIGFLAGS)"
	@echo "======================================================================"

## ----------------------------------------------------------------------
## Sync/Clean tools
## ----------------------------------------------------------------------
## Populate/clean up dependent releng supported tools.  The projects are
## downloaded and installed into /opt/releng/...
##
## Tool dependencies and platform config mappings are defined in:
##   * Apache Ivy dependency definition file
##       releng/make/dependencies/ivy.xml
## ----------------------------------------------------------------------

opt_write_test:
	@if [ ! -w /opt ]; then \
	    echo ""; \
	    echo "======================================================================"; \
	    echo "ERROR: /opt is not writable."; \
	    echo "----------------------------------------------------------------------"; \
	    echo "  Supporting tools are stored in /opt.  Please ensure you have"; \
	    echo "  write access to /opt"; \
	    echo "======================================================================"; \
	    echo ""; \
	    exit 1; \
	fi

/opt/releng/apache-ant: 
	${MAKE} opt_write_test
	echo "Sync Ivy project dependency management framework ..."
	curl --silent http://releng.sanmateo.greenplum.com/tools/apache-ant.1.8.1.tar.gz -o /tmp/apache-ant.1.8.1.tar.gz
	( umask 002; [ ! -d /opt/releng ] && mkdir -p /opt/releng; \
	   cd /opt/releng; \
	   gunzip -qc /tmp/apache-ant.1.8.1.tar.gz | tar xf -; \
	   rm -f /tmp/apache-ant.1.8.1.tar.gz; \
	   chmod -R a+w /opt/releng/apache-ant )

sync_tools: opt_write_test /opt/releng/apache-ant
	@cd releng/make/dependencies; \
	 (umask 002; /opt/releng/apache-ant/bin/ant -DBLD_ARCH=$(BLD_ARCH) resolve); \
	 echo "Resolve finished"

clean_tools: opt_write_test
	@cd releng/make/dependencies; \
	/opt/releng/apache-ant/bin/ant clean; \
	rm -rf /opt/releng/apache-ant; \
