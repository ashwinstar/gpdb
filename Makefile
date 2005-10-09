MPPPGDIR=cdb-pg
MPPMGMT=mppMgmt
ISCONFIG=$(MPPPGDIR)/GNUmakefile

RELEASE=`TMP=\`cat releasetag | awk 'BEGIN {FS = ":"} {print $$2}'\`;TMP2=$${TMP%% \\$$}; echo $$TMP2`
MPPDIR=mpp-pgsql-$(RELEASE)

TARBALL=/tmp/$(MPPDIR).tar.gz

DEFPORT=5432

OPTFLAGS="-O3 -funroll-loops -fomit-frame-pointer -fargument-noalias-global $(COPTX)"
DEBUGFLAGS="-O0 -g"

DISTPATH=$(BIZHOME)/$(MPPDIR)
DEVPATH=$(HOME)/$(MPPDIR)

CONFIGFLAGS=--enable-thread-safety --enable-multibyte=UNICODE --with-maxbackends=1024 --enable-integer-datetimes --with-pgport=$(DEFPORT)

all : devel

.PHONY : RECONFIG BIZDEP HOMEDEP clean distclean

devel : INSTCFLAGS=$(DEBUGFLAGS)
devel : INSTLOC=$(DEVPATH)
devel : CONFIGFLAGS+=--enable-cassert --enable-debug
devel : HOMEDEP $(ISCONFIG)
	@rm -rf $(INSTLOC)
	@cd $(MPPPGDIR); make install
	@make mgmtcopy INSTLOC=$(INSTLOC)

dist : INSTCFLAGS=$(OPTFLAGS)
dist : INSTLOC=$(DISTPATH)
dist : BIZDEP RECONFIG $(ISCONFIG)
	@rm -rf $(INSTLOC)
	@cd $(MPPPGDIR); make install
	@make mgmtcopy INSTLOC=$(INSTLOC)
	@rm -f $(TARBALL); make $(TARBALL) INSTLOC=$(INSTLOC)

#$(TARBALL) :
#	@gtar czf $(TARBALL) $(INSTLOC)

mgmtcopy :
	@#Copy the management utilities
	@mkdir $(INSTLOC)/$(MPPMGMT)
	@cp -rp $(MPPMGMT)/bin $(INSTLOC)/$(MPPMGMT)
	@if [ -d $(MPPMGMT)/doc ]; then cp -rp $(MPPMGMT)/doc $(INSTLOC)/$(MPPMGMT); fi
	@if [ -d $(MPPMGMT)/demo/mppdemo ]; then mkdir $(INSTLOC)/$(MPPMGMT)/demo; gtar -C $(MPPMGMT)/demo -czf $(INSTLOC)/$(MPPMGMT)/demo/mppdemo.tar.gz mppdemo; fi
	@rm -rf $(INSTLOC)/$(MPPMGMT)/bin/CVS
	@rm -rf $(INSTLOC)/$(MPPMGMT)/doc/CVS

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
	@rm -f $(TARBALL)
	@cd $(MPPPGDIR); make clean

distclean :
	@if [ ! -f $(ISCONFIG) ]; then echo "not configured, no need to clean"; exit 1; fi
	@cd $(MPPPGDIR); make distclean
