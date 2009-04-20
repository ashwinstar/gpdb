#         $Id: $
#     $Change: $
#   $DateTime: $
#     $Author: $

# protect this makefile's default target from targets in Makefile.global and Makefile.thirdparty
default: all

BLD_TOP=../..
include $(BLD_TOP)/Makefile.global
include $(BLD_TOP)/Makefile.thirdparty

SHELL=bash
CC=gcc
GPCC=$(shell echo `pwd`/..)
CDBINCL=-I$(GPCC)/../cdb-pg/src/include
OPT=-g -O3
ifeq "$(BLD_ARCH)" "win32"
APR=apr-1.3.3
APU=apr-util-1.3.4
else
APR=apr-1.2.12
APU=apr-util-1.2.12
endif
JSON=json-c-0.6
LIBEVENT=libevent-1.4.6-stable
OPENSSL=openssl-0.9.8h

ifeq "no-match$(findstring $(BLD_ARCH),osx105_x86 suse10_x86_64)" "no-match"
# continue to use this ext/ directory on the remaining platforms
all: mkdir mklibjson mklibevent mklibapr mklibapu mkdbgen
else
# use ../../ext/ directory, instead, on these platforms
all: mkdir mkdbgen
endif

ifeq "$(BLD_TARGET)" ""
CONFIG_OPENSSL=./config
else
CONFIG_OPENSSL=./Configure $(BLD_TARGET)
endif

mkdir:
	@echo --- ext ------------------------------------------------------
	@echo $(GPCC)
	mkdir -p lib include

mklibjson: mkdir lib/libjson.a
mklibevent: mkdir lib/libevent.a
mklibssl: mkdir lib/libssl.a
mklibapr: mkdir lib/libapr-1.a
mklibapu: mkdir lib/libaprutil-1.a

lib/libjson.a:
	@echo     -- json
	if ! [ -a $(JSON)/Makefile ]; then \
		$(TAR) xzf $(JSON).tar.gz;\
		(cd $(JSON) && CC="$(CC)" LDFLAGS="$(LDFLAGS)" \
		./configure --enable-shared=no \
			$(CONFFLAGS) \
			LDFLAGS=$(LDFLAGS) \
			CC=$(CC) \
			CPPFLAGS=$(CPPFLAGS) \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	(cd $(JSON) && $(MAKE) all && $(MAKE) install >> make.out)	


lib/libevent.a:
	@echo     -- libevent
	if ! [ -a $(LIBEVENT)/Makefile ]; then \
		$(TAR) xzf $(LIBEVENT).tar.gz;\
		(cd $(LIBEVENT) && CC="$(CC)" LDFLAGS="$(LDFLAGS)" ./configure --enable-shared=no \
			$(CONFFLAGS) \
			LDFLAGS=$(LDFLAGS) \
			CC=$(CC) \
			CPPFLAGS=$(CPPFLAGS) \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	(cd $(LIBEVENT) && $(MAKE) install >> make.out)

lib/libssl.a:
	@echo     -- openssl
	if ! [ -a $(OPENSSL)/Makefile ]; then \
		$(TAR) xzf $(OPENSSL).tar.gz;\
		(cd $(OPENSSL) && CC="$(CC)" LDFLAGS="$(LDFLAGS)" $(CONFIG_OPENSSL) no-shared threads --prefix=$(GPCC)/ext  \
			CPPFLAGS="$(CPPFLAGS)" $(OSFLAGS)) \
				;\
	fi
	(cd $(OPENSSL) && $(MAKE)  all install >> make.out 2>&1)

lib/libaprutil-1.a: lib/libapr-1.a
	@echo     -- apu
	if ! [ -a $(APU)/Makefile ]; then \
		$(TAR) xzf $(APU).tar.gz;\
		(cd $(APU) && CC="$(CC)" LDFLAGS="$(LDFLAGS)" ./configure --enable-shared=no \
			$(CONFFLAGS) \
			LDFLAGS=$(LDFLAGS) \
			CC=$(CC) \
			CPPFLAGS=$(CPPFLAGS) \
			--enable-static=yes --enable-threads \
			--with-apr=../$(APR) \
			--prefix=$(GPCC)/ext) ;\
	fi
	(cd $(APU) && $(MAKE) all install >> make.out)


lib/libapr-1.a:
	@echo     -- apr
	if ! [ -a $(APR)/Makefile ]; then \
		$(TAR) xzf $(APR).tar.gz;\
		patch $(APR)/configure.in < apr1.patch; \
		cd $(APR) ; \
		autoconf ; \
		cd .. ; \
		(cd $(APR) && CC="$(CC)" LDFLAGS="$(LDFLAGS)" ./configure --enable-shared=no \
			$(CONFFLAGS) \
			LDFLAGS=$(LDFLAGS) \
			CC=$(CC) \
			CPPFLAGS=$(CPPFLAGS) \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	(cd $(APR) && $(MAKE) all install >> make.out)

mkdbgen:
	@echo     -- dbgen
	cd tpch && $(MAKE) CC="$(CC)" LDFLAGS="$(LDFLAGS)"
	cd tpch && cp -f dbgen ../../build/bin/
	cd tpch && cp -f dists.dss ../../build/bin/

veryclean:
	rm -rf lib bin include build1 share ssl build-1
	rm -rf $(APR) $(APU) $(JSON) $(LIBEVENT) $(OPENSSL)

clean:
	rm -rf lib bin include build1 share ssl build-1
	(cd $(APR) && $(MAKE) clean; rm -f make.out)
	(cd $(APU) && $(MAKE) clean; rm -f make.out)
	(cd $(JSON) && $(MAKE) clean; rm -f make.out)
	(cd $(LIBEVENT) && $(MAKE) clean; rm -f make.out)
	(cd $(OPENSSL) && $(MAKE) clean; rm -f make.out)

