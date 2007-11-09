GPPGDIR=cdb-pg
GPMGMT=gpMgmt
GPCC=gpcc
ISCONFIG=$(GPPGDIR)/GNUmakefile

RELEASE=$(shell $(CURDIR)/releasetag)
GPDIR=greenplum-db$(RELEASE)
GPDOCDIR=$(CURDIR)/docs/release

DEFPORT=5432
INLINE_FLAGS=--param max-inline-insns-single=800 --param large-function-growth=175 --param inline-unit-growth=300 -Winline
OPTFLAGS="-O3 -funroll-loops -fargument-noalias-global $(COPTX) -g $(INLINE_FLAGS)"

ifeq (on, ${GPDBGINLINE})
CFLAGS_INLINE=$(INLINE_FLAGS)
else
CFLAGS_INLINE=-fno-inline
endif

ifeq (on, ${GPDBGOPT})
CFLAGS_OPT=-O1
else
CFLAGS_OPT=-O0
endif

DEBUGFLAGS="$(CFLAGS_INLINE) $(CFLAGS_OPT) -g3"

DISTPATH=$(GPROOT)/$(GPDIR)
DEVPATH=$(HOME)/$(GPDIR)

CONFIGFLAGS=--enable-thread-safety --enable-integer-datetimes --with-pgport=$(DEFPORT) 

ifeq (true, ${PG_LANG})
CONFIGFLAGS+=--with-perl
ifdef TCL_CFG
CONFIGFLAGS+=--with-tcl-config=${TCL_CFG}
endif
endif

ifeq (true, ${AUTH})
CONFIGFLAGS+=--with-openssl --with-pam --with-krb5 --with-ldap --with-libraries="$(CURDIR)/curl/lib $(CURDIR)/openldap/lib $(CURDIR)/openssl/lib $(CURDIR)/krb5/lib" --with-includes="$(CURDIR)/curl/include $(CURDIR)/openldap/include $(CURDIR)/openssl/include $(CURDIR)/krb5/include"
endif

all : devel

.PHONY : RECONFIG GPROOTDEP HOMEDEP clean distclean distprep version

test2 :
	echo "CWD ="$(CURDIR)
	echo "RELEASE="$(RELEASE)

devel : INSTCFLAGS=$(DEBUGFLAGS)
devel : INSTLOC=$(DEVPATH)
devel : CONFIGFLAGS+=--enable-cassert --enable-debug
devel : gccVersionCheck version HOMEDEP $(ISCONFIG)
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
dist : gccVersionCheck version GPROOTDEP RECONFIG $(ISCONFIG)
	@rm -rf $(INSTLOC)
	@cd $(GPPGDIR); $(MAKE) install
	@$(MAKE) groupSession
	@$(MAKE) mkgpcc
	@$(MAKE) mgmtcopy INSTLOC=$(INSTLOC)
	@$(MAKE) copydocs INSTLOC=$(INSTLOC)
	@$(MAKE) copylibs INSTLOC=$(INSTLOC)

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


mgmtcopy :
	@#Copy the management utilities
	@mkdir -p $(INSTLOC)/bin
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
	if [ `uname -s` = 'SunOS' ]; then cp `ldd $(INSTLOC)/bin/psql | grep gcc | head -1 | awk '{print $$3}'` $(INSTLOC)/lib; fi	
	if [ -f $(CURDIR)/krb5/lib/libk5crypto.so.3 ] ; then cp -p $(CURDIR)/krb5/lib/libk5crypto.so.3 $(INSTLOC)/lib ; fi
	if [ -f $(CURDIR)/openldap/lib/libldap_r-2.3.so.0 ] ; then cp -p $(CURDIR)/openldap/lib/libldap_r-2.3.so.0 $(INSTLOC)/lib ; fi
	if [ -f $(CURDIR)/openldap/lib/libldap-2.3.so.0 ] ; then cp -p $(CURDIR)/openldap/lib/libldap-2.3.so.0 $(INSTLOC)/lib ; fi
	if [ `uname -s` = 'Darwin' ] ; then \
	    for lib in `otool -L $(INSTLOC)/bin/psql | egrep 'libssl|libcrypto|libcom_err|libkrb5|curl|readline|numa' | awk '{print $$1}'`; do \
	        echo "cp -p $$lib $(INSTLOC)/lib" ; \
	        cp -p $$lib $(INSTLOC)/lib ; \
	    done ; \
	fi
	if [ `uname -s` != 'Darwin' ] ; then \
	    for lib in `ldd $(INSTLOC)/bin/psql | egrep 'libssl|libcrypto|libcom_err|libkrb5|curl|readline|numa' | awk '{print $$3}' | sort -u`; do \
	        echo "cp -p $$lib $(INSTLOC)/lib" ; \
	        cp -p $$lib $(INSTLOC)/lib ; \
	    done ; \
	fi

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

gccVersionCheck :
	@echo "Checking GCC version" ; \
	gcc --version > /dev/null 2>&1 ; \
	if [ $$? -ne 0 ] ; then \
 	    echo "Error running gcc --version" ; \
	    gcc --version ; \
	    exit 1 ; \
	fi
	@gcc --version | head -n 1 | cut -d' ' -f3 | egrep '[0-3]\.[0-4]\.[0-1]' > /dev/null 2>&1 ; \
	if [ $$? -eq 0 ] ; then \
	    echo "Error: GPDB must be compiled with GCC 3.4.2 or later." ; \
	    echo "Current GCC version:" ; \
	    gcc --version ; \
	    exit 1 ; \
	fi

pygres: GPROOTDEP
	rm -rf PyGreSQL-3.8.1
	cvs co ThirdPartyTools/src/PyGreSQL-3.8.1 > pygres-3.8.1-co.log 2>&1
	(cd ThirdPartyTools/src/PyGreSQL-3.8.1 && \
	python setup.py build --build-lib=libs && \
	if test -d $(DISTPATH)/lib ; then \
	    cp -p libs/* $(DISTPATH)/lib ; \
	else \
	    cp -p libs/* $(DEVPATH)/lib ; \
	fi)

authlibs: GPROOTDEP
	cvs co -d openldap-2.3.38 ThirdPartyTools/src/openldap-2.3.38 > openldap-2.3.38-co.log 2>&1
	cvs co -d openssl-0.9.8g ThirdPartyTools/src/openssl-0.9.8g > openlssl-0.9.8g-co.log 2>&1
	cvs co -d krb5-1.6.2 ThirdPartyTools/src/krb5-1.6.2 > krb5-1.6.2-co.log 2>&1
	cvs co -d curl-7.17.1 ThirdPartyTools/curl-7.17.1 > curl-7.17.1-co.log 2>&1

	@echo "Building libraries required for authentication"

	@echo "Building openssl"
	(cd ${CURDIR}/openssl-0.9.8g && ./config --prefix=$(CURDIR)/openssl shared && make && make install)

	@echo "Building openldap"
	(cd ${CURDIR}/openldap-2.3.38 && ./configure LDFLAGS="-L$(CURDIR)/openssl/lib" CPPFLAGS="-I$(CURDIR)/openssl/include" --prefix=$(CURDIR)/openldap --enable-slapd=no && make && make install)

	@echo "Building krb5"
	(cd ${CURDIR}/krb5-1.6.2/src && ./configure --prefix=$(CURDIR)/krb5 --without-tcl && make && make install)

	@echo "Building curl"
	(cd $(CURDIR)/curl-7.17.1 && ./configure --prefix=$(CURDIR)/curl --disable-ldap --disable-ipv6 --without-ssl && make && make install)

	@echo ""
	@echo "openssl installed at $(CURDIR)/openssl"
	@echo "openldap installed at $(CURDIR)/openldap"
	@echo "krb5 installed at $(CURDIR)/krb5"
	@echo ""

	@echo "export LD_LIBRARY_PATH=$(CURDIR)/curl/lib:$(CURDIR)/openssl/lib:$(CURDIR)/openldap/lib:$(CURDIR)/krb5/lib:\$$LD_LIBRARY_PATH" > authlib_libpath_setup.sh
	@echo "export DYLD_LIBRARY_PATH=$(CURDIR)/curl/lib:$(CURDIR)/openssl/lib:$(CURDIR)/openldap/lib:$(CURDIR)/krb5/lib:\$$DYLD_LIBRARY_PATH" >> authlib_libpath_setup.sh

	@echo "To add the auth libs to your LD_LIBRARY_PATH or DYLD_LIBRARY_PATH source authlib_libpath_setup.sh"
