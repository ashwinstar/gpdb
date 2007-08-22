GPPGDIR=cdb-pg
GPMGMT=gpMgmt
GPCC=gpcc
ISCONFIG=$(GPPGDIR)/GNUmakefile

RELEASE=`$(CURDIR)/releasetag`
GPDIR=greenplum-db$(RELEASE)
GPDOCDIR=$(CURDIR)/docs/release

TARBALL=/tmp/$(GPDIR).tar.gz

DEFPORT=5432

OPTFLAGS="-O3 -finline-limit=1800 -funroll-loops -fargument-noalias-global $(COPTX) -g"
DEBUGFLAGS="-O0 -g"

DISTPATH=$(GPROOT)/$(GPDIR)
DEVPATH=$(HOME)/$(GPDIR)

CONFIGFLAGS=--enable-thread-safety --enable-integer-datetimes --with-pgport=$(DEFPORT) 

ifeq (true, ${PG_LANG})
CONFIGFLAGS+=--with-perl
ifdef TCL_CFG
CONFIGFLAGS+=--with-tcl-config=${TCL_CFG}
endif
endif

all : devel

.PHONY : RECONFIG GPROOTDEP HOMEDEP clean distclean tarball distprep version

test2 :
	echo "CWD ="$(CURDIR)
	echo "RELEASE="$(RELEASE)

devel : INSTCFLAGS=$(DEBUGFLAGS)
devel : INSTLOC=$(DEVPATH)
devel : CONFIGFLAGS+=--enable-cassert --enable-debug
devel : version HOMEDEP $(ISCONFIG)
	@rm -rf $(INSTLOC)
	@cd $(GPPGDIR); $(MAKE) install
	@$(MAKE) groupSession
	@$(MAKE) mkgpcc
	@$(MAKE) mgmtcopy INSTLOC=$(INSTLOC)
	@$(MAKE) copylibs INSTLOC=$(INSTLOC)

forceplan : INSTCFLAGS=$(DEBUGFLAGS)
forceplan : INSTLOC=$(DEVPATH)
forceplan : CONFIGFLAGS+=--enable-cassert --enable-debug --enable-forceplan --enable-debugbreak --enable-debugntuplestore
forceplan : HOMEDEP $(ISCONFIG)
	@rm -rf $(INSTLOC)
	@cd $(GPPGDIR); $(MAKE) install
	@$(MAKE) groupSession
	@$(MAKE) mkgpcc
	@$(MAKE) mgmtcopy INSTLOC=$(INSTLOC)
	@$(MAKE) copylibs INSTLOC=$(INSTLOC)

dist : INSTCFLAGS=$(OPTFLAGS)
dist : INSTLOC=$(DISTPATH)
dist : version GPROOTDEP RECONFIG $(ISCONFIG)
	@rm -rf $(INSTLOC)
	@cd $(GPPGDIR); $(MAKE) install
	@$(MAKE) groupSession
	@$(MAKE) mkgpcc
	@$(MAKE) mgmtcopy INSTLOC=$(INSTLOC)
	@$(MAKE) copydocs INSTLOC=$(INSTLOC)
	@$(MAKE) copylibs INSTLOC=$(INSTLOC)
	@rm -f $(TARBALL); $(MAKE) tarball TARBALL=$(TARBALL) INSTLOC=$(INSTLOC)

groupSession:
	@cd $(GPMGMT)/bin; $(MAKE) 

mkgpcc:
	@cd $(GPCC); $(MAKE)


copydocs :
	@if [ ! -d $($INSTLOC)/docs ] ; then mkdir -p $(INSTLOC)/docs; fi
	@if [ ! -d $(GPDOCDIR) ] ; then \
	echo " Error Copying Documentation: $(GPDOCDIR) is not present"; \
	else  echo "Copying Documentation" ;  \
	echo "Copying  $(GPDOCDIR)/README "; \
	cp $(GPDOCDIR)/README $(INSTLOC)/docs/README ; \
	echo "Copying $(GPDOCDIR)/pdf/GP*.pdf " ; \
	cp $(GPDOCDIR)/pdf/GP*.pdf $(INSTLOC)/docs/; \
	fi 

tarball :
	@gtar czf $(TARBALL) $(INSTLOC)

mgmtcopy :
	@#Copy the management utilities
	@cp -rp $(GPMGMT)/bin $(INSTLOC)
	@cp -rp $(GPCC)/build/bin $(INSTLOC)
	@cp $(GPPGDIR)/src/test/regress/*.pl $(INSTLOC)/bin
	@if [ ! -d ${INSTLOC}/docs ] ; then mkdir ${INSTLOC}/docs ; fi
	@if [ -d $(GPMGMT)/doc ]; then cp -rp $(GPMGMT)/doc $(INSTLOC)/docs/cli_help; fi
	@if [ -d $(GPMGMT)/demo/gpdemo ]; then mkdir $(INSTLOC)/demo; gtar --exclude \*CVS\* -C $(GPMGMT)/demo -czf $(INSTLOC)/demo/gpdemo.tar.gz gpdemo; fi
	@# Set version
	@perl putversion \
		$(INSTLOC)/bin/gpactivatestandby \
		$(INSTLOC)/bin/gpaddmirrors \
		$(INSTLOC)/bin/gpaddpart \
		$(INSTLOC)/bin/gpcheckdb \
		$(INSTLOC)/bin/gpcheckos \
		$(INSTLOC)/bin/gpcheckperf \
		$(INSTLOC)/bin/gpcreatepart \
		$(INSTLOC)/bin/gpcrondump \
		$(INSTLOC)/bin/gpdbrestore \
		$(INSTLOC)/bin/gpdeletepart \
		$(INSTLOC)/bin/gpdeletesystem \
		$(INSTLOC)/bin/gpinitstandby \
		$(INSTLOC)/bin/gpinitsystem \
		$(INSTLOC)/bin/gprebuildseg \
		$(INSTLOC)/bin/gprebuildsystem \
		$(INSTLOC)/bin/gprecoverseg \
		$(INSTLOC)/bin/gpscp \
		$(INSTLOC)/bin/gpscp2 \
		$(INSTLOC)/bin/gpsizecalc \
		$(INSTLOC)/bin/gpskew \
		$(INSTLOC)/bin/gpssh \
		$(INSTLOC)/bin/gpssh-exkeys \
		$(INSTLOC)/bin/gpstart \
		$(INSTLOC)/bin/gpstate \
		$(INSTLOC)/bin/gpstop \
		$(INSTLOC)/bin/gpsys1
	@# Remove unwanted files.
	@rm -rf $(INSTLOC)/bin/CVS
	@rm -rf $(INSTLOC)/doc/CVS
	@rm -rf $(INSTLOC)/bin/pythonSrc
	@rm -rf $(INSTLOC)/bin/Makefile
	@rm -rf $(INSTLOC)/bin/lib/CVS
	@rm -rf $(INSTLOC)/bin/src

copylibs :
	if [ `uname -s` = 'SunOS' ]; then cp `ldd $(INSTLOC)/bin/psql | grep readline | head -1 | awk '{print $$3}'` $(INSTLOC)/lib; fi
	if [ `uname -s` = 'SunOS' ]; then cp `ldd $(INSTLOC)/bin/psql | grep gcc | head -1 | awk '{print $$3}'` $(INSTLOC)/lib; fi
	if [ -f /usr/local/lib/libcurl.so.4 ] ; then cp -p /usr/local/lib/libcurl.so.4 $(INSTLOC)/lib; fi
	if [ -f /usr/local/lib/libcurl.4.dylib ] ; then cp -p /usr/local/lib/libcurl.4.dylib $(INSTLOC)/lib; fi
	if [ `uname -s` = 'Linux' ]; then cp `ldd $(INSTLOC)/bin/psql | grep numa | head -1 | awk '{print $$3}'` $(INSTLOC)/lib; fi

RECONFIG :
	rm -f $(ISCONFIG)

$(ISCONFIG) :
	cd $(GPPGDIR); CFLAGS=$(INSTCFLAGS) ./configure $(CONFIGFLAGS) --prefix=$(INSTLOC) --with-docdir=$(INSTLOC)/doc --mandir=$(INSTLOC)/man

HOMEDEP :
	@#If the HOME variable isn't set, then execute the usage() path
	@if [ x"$(HOME)" = x ]; then $(MAKE) HOMEFAIL; fi

GPROOTDEP :
	@#If the GPROOT variable isn't set, then execute the usage() path
	@if [ x"$(GPROOT)" = x ]; then $(MAKE) GPROOTFAIL; fi

GPROOTFAIL :
	@echo
	@echo "	The GPROOT path variable is not set."
	@echo
	@echo "	You must decide where you will install to, and set GPROOT to that location, prior to"
	@echo "	installing the GP module with this Makefile."
	@echo
	@exit 1

HOMEFAIL :
	@echo
	@echo "	The HOME environment variable is not set. Please set it to continue."
	@echo
	@exit 1

clean :
	@if [ ! -f $(ISCONFIG) ]; then echo "not configured, no need to clean"; exit 1; fi
	@cd $(GPPGDIR); $(MAKE) clean
	@cd $(GPMGMT)/bin; $(MAKE) clean
	@cd $(GPCC); $(MAKE) clean
	@rm -f $(TARBALL)

distclean :
	@if [ ! -f $(ISCONFIG) ]; then echo "not configured, no need to clean"; exit 1; fi
	@cd $(GPPGDIR); $(MAKE) distclean
	@cd $(GPMGMT)/bin; $(MAKE) clean
	@cd $(GPCC); $(MAKE) clean
	
distprep : $(ISCONFIG)
	@cd $(GPPGDIR); $(MAKE) distprep
	
installcheck-good :
	@if [ ! -f $(ISCONFIG) ]; then echo "not configured, cannot check"; exit 1; fi
	@cd $(GPPGDIR)/src/test/regress; $(MAKE) installcheck-good

installcheck-olaptest :
	@if [ ! -f $(ISCONFIG) ]; then echo "not configured, cannot check"; exit 1; fi
	@cd $(GPPGDIR)/src/test/regress; $(MAKE) installcheck-olaptest

version :
	@if [ ! -f getversion ] ; then echo "getversion is not present, cannot create VERSION file"; exit 1; fi
	@echo "Creating VERSION file"
	@bash getversion > VERSION
