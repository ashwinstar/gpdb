MPPPGDIR=cdb-pg
MPPMGMT=mppMgmt
ISCONFIG=$(MPPPGDIR)/GNUmakefile

#MPPCFLAGS=-O3 -funroll-loops -fomit-frame-pointer -fargument-noalias-global $(COPTX)
MPPCFLAGS=-O0 -g

DISTPATH=$(BIZHOME)/mpp-pgsql-2.0

DOCDIR=$(DISTPATH)/doc
DEFPORT=10000

CONFIGFLAGS=--prefix=$(DISTPATH) --enable-thread-safety --enable-multibyte=UNICODE --with-maxbackends=1024 --enable-integer-datetimes --with-docdir=$(DOCDIR) --mandir=$(DOCDIR)/man --with-pgport=$(DEFPORT)

all: BIZHOMESET $(ISCONFIG) 
	@cd $(MPPPGDIR); make

.PHONY: BIZHOMESET

BIZHOMESET:
	@#If the BIZHOME variable isn't set, then execute the usage() path
	@if [ x"$(BIZHOME)" = x ]; then make BIZDEP; fi

dist:
	@rm -rf $(DISTPATH)
	@cd $(MPPPGDIR); make install
	@#Copy the management utilities
	@mkdir $(DISTPATH)/$(MPPMGMT)
	@cp -rp $(MPPMGMT)/bin $(DISTPATH)/$(MPPMGMT)
	@cp -rp $(MPPMGMT)/doc $(DISTPATH)/$(MPPMGMT)
	@rm -rf $(DISTPATH)/$(MPPMGMT)/bin/CVS
	@rm -rf $(DISTPATH)/$(MPPMGMT)/doc/CVS

$(ISCONFIG):
	cd $(MPPPGDIR); CFLAGS=$(MPPCFLAGS) ./configure $(CONFIGFLAGS)

BIZDEP:
	@echo
	@echo "	The BIZHOME path variable is not set."
	@echo
	@echo "	You must install bizgres first, then source the file with path variables prior to"
	@echo "	installing the MPP module with this Makefile."
	@echo
	@echo "	If you want a developer build, go to the directory $(MPPPGDIR) and configure/make from there."
	@echo
	@exit 1

.PHONY: clean

clean:
	@cd $(MPPPGDIR); make clean

.PHONY: distclean

distclean:
	@cd $(MPPPGDIR); make distclean
