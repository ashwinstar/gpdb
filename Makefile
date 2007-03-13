MPPPGDIR=cdb-pg
MPPMGMT=mppMgmt
ISCONFIG=$(MPPPGDIR)/GNUmakefile

RELEASE=`$(CURDIR)/releasetag`
MPPDIR=gp-pgsql$(RELEASE)
MPPDOCDIR=$(CURDIR)/docs/release

TARBALL=/tmp/$(MPPDIR).tar.gz

DEFPORT=5432

OPTFLAGS="-O3 -finline-limit=1800 -funroll-loops -fargument-noalias-global $(COPTX) -g"
DEBUGFLAGS="-O0 -g"

DISTPATH=$(BIZHOME)/$(MPPDIR)
DEVPATH=$(HOME)/$(MPPDIR)

CONFIGFLAGS=--enable-thread-safety --enable-integer-datetimes --with-pgport=$(DEFPORT) --enable-depend 

ifeq (true, ${PG_LANG})
CONFIGFLAGS+=--with-python --with-perl --with-tcl
ifdef TCL_CFG
CONFIGFLAGS+=--with-tcl-config=${TCL_CFG}
endif
endif

all : devel

.PHONY : RECONFIG BIZDEP HOMEDEP clean distclean tarball distprep

test2 :
	echo "CWD ="$(CURDIR)
	echo "RELEASE="$(RELEASE)

devel : INSTCFLAGS=$(DEBUGFLAGS)
devel : INSTLOC=$(DEVPATH)
devel : CONFIGFLAGS+=--enable-cassert --enable-debug
devel : HOMEDEP $(ISCONFIG)
	@rm -rf $(INSTLOC)
	@cd $(MPPPGDIR); make install
	@make groupSession
	@make mgmtcopy INSTLOC=$(INSTLOC)
	@make copylibs INSTLOC=$(INSTLOC)

dist : INSTCFLAGS=$(OPTFLAGS)
dist : INSTLOC=$(DISTPATH)
dist : BIZDEP RECONFIG $(ISCONFIG)
	@rm -rf $(INSTLOC)
	@cd $(MPPPGDIR); make install
	@make groupSession
	@make mgmtcopy INSTLOC=$(INSTLOC)
	@make copydocs INSTLOC=$(INSTLOC)
	@make copylibs INSTLOC=$(INSTLOC)
	@rm -f $(TARBALL); make tarball TARBALL=$(TARBALL) INSTLOC=$(INSTLOC)

groupSession:
	@cd $(MPPMGMT)/bin; make 


copydocs :
	@if [ ! -d $($INSTLOC)/docs ] ; then mkdir -p $(INSTLOC)/docs; fi
	@if [ ! -d $(MPPDOCDIR) ] ; then \
	echo " Error Copying Documentation: $(MPPDOCDIR) is not present"; \
	else  echo "Copying Documentation" ;  \
	echo "Copying  $(MPPDOCDIR)/README "; \
	cp $(MPPDOCDIR)/README $(INSTLOC)/docs/README ; \
	echo "Copying $(MPPDOCDIR)/pdf/GP*.pdf " ; \
	cp $(MPPDOCDIR)/pdf/GP*.pdf $(INSTLOC)/docs/; \
	fi 

tarball :
	@gtar czf $(TARBALL) $(INSTLOC)

mgmtcopy :
	@#Copy the management utilities
	@mkdir $(INSTLOC)/$(MPPMGMT)
	@cp -rp $(MPPMGMT)/bin $(INSTLOC)/$(MPPMGMT)
	@if [ -d $(MPPMGMT)/doc ]; then cp -rp $(MPPMGMT)/doc $(INSTLOC)/$(MPPMGMT); fi
	@if [ -d $(MPPMGMT)/demo/mppdemo ]; then mkdir $(INSTLOC)/$(MPPMGMT)/demo; gtar --exclude \*CVS\* -C $(MPPMGMT)/demo -czf $(INSTLOC)/$(MPPMGMT)/demo/mppdemo.tar.gz mppdemo; fi
	@# Remove unwanted files.
	@rm -rf $(INSTLOC)/$(MPPMGMT)/bin/CVS
	@rm -rf $(INSTLOC)/$(MPPMGMT)/doc/CVS
	@rm -rf $(INSTLOC)/$(MPPMGMT)/bin/pythonSrc
	@rm -rf $(INSTLOC)/$(MPPMGMT)/bin/Makefile
	@rm -rf $(INSTLOC)/$(MPPMGMT)/bin/lib/CVS

copylibs :
	if [ `uname -s` = 'SunOS' ]; then cp `ldd $(INSTLOC)/bin/psql | grep readline | head -1 | awk '{print $$3}'` $(INSTLOC)/lib; fi
	if [ `uname -s` = 'SunOS' ]; then cp `ldd $(INSTLOC)/bin/psql | grep gcc | head -1 | awk '{print $$3}'` $(INSTLOC)/lib; fi
	if [ -f /usr/local/lib/libcurl.so.4 ] ; then cp -p /usr/local/lib/libcurl.so.4 $(INSTLOC)/lib; fi

RECONFIG :
	rm -f $(ISCONFIG)

$(ISCONFIG) :
	cd $(MPPPGDIR); CFLAGS=$(INSTCFLAGS) ./configure $(CONFIGFLAGS) --prefix=$(INSTLOC) --with-docdir=$(INSTLOC)/doc --mandir=$(INSTLOC)/man

HOMEDEP :
	@#If the BIZHOME variable isn't set, then execute the usage() path
	@if [ x"$(HOME)" = x ]; then make HOMEFAIL; fi

BIZDEP :
	@#If the BIZHOME variable isn't set, then execute the usage() path
	@if [ x"$(BIZHOME)" = x ]; then make BIZFAIL; fi

BIZFAIL :
	@echo
	@echo "	The BIZHOME path variable is not set."
	@echo
	@echo "	You must install bizgres first, then source the file with path variables prior to"
	@echo "	installing the MPP module with this Makefile."
	@echo
	@exit 1

HOMEFAIL :
	@echo
	@echo "	The HOME environment variable is not set. Please set it to continue."
	@echo
	@exit 1

clean :
	@if [ ! -f $(ISCONFIG) ]; then echo "not configured, no need to clean"; exit 1; fi
	@cd $(MPPPGDIR); make clean
	@rm -f $(TARBALL)

distclean :
	@if [ ! -f $(ISCONFIG) ]; then echo "not configured, no need to clean"; exit 1; fi
	@cd $(MPPPGDIR); make distclean
	
distprep : $(ISCONFIG)
	@cd $(MPPPGDIR); make distprep
