#         $Id: $
#     $Change: $
#   $DateTime: $
#     $Author: $

SHELL=bash
CC=gcc
GPCC=$(shell echo `pwd`/..)
CDBINCL=-I$(GPCC)/../cdb-pg/src/include
OPT=-g -O3
APR=apr-1.2.12
APU=apr-util-1.2.12
JSON=json-c-0.6
LIBEVENT=libevent-1.4.6-stable
OPENSSL=openssl-0.9.8h

ifeq "no-match$(findstring $(BLD_ARCH),suse10_x86_64)" "no-match"
all: mkdir mkdbgen
else
all: mkdir mklibjson mklibevent mklibapr mklibapu mkdbgen
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
		gzip -d -c $(JSON).tar.gz | tar xvf - ;\
		(cd $(JSON) && ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	(cd $(JSON) && $(MAKE) all && $(MAKE) install >> make.out)	


lib/libevent.a:
	@echo     -- libevent
	if ! [ -a $(LIBEVENT)/Makefile ]; then \
		gzip -d -c $(LIBEVENT).tar.gz | tar xvf - ;\
		(cd $(LIBEVENT) && ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	(cd $(LIBEVENT) && $(MAKE) install >> make.out)

lib/libssl.a:
	@echo     -- openssl
	if ! [ -a $(OPENSSL)/Makefile ]; then \
		gzip -d -c $(OPENSSL).tar.gz | tar xvf - ;\
		(cd $(OPENSSL) && ./config no-shared threads --prefix=$(GPCC)/ext) ;\
	fi
	(cd $(OPENSSL) && $(MAKE)  all install >> make.out 2>&1)

lib/libaprutil-1.a: lib/libapr-1.a
	@echo     -- apu
	if ! [ -a $(APU)/Makefile ]; then \
		gzip -d -c $(APU).tar.gz | tar xvf - ;\
		(cd $(APU) && ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--with-apr=../$(APR) \
			--prefix=$(GPCC)/ext) ;\
	fi
	(cd $(APU) && $(MAKE) all install >> make.out)


lib/libapr-1.a:
	@echo     -- apr
	if ! [ -a $(APR)/Makefile ]; then \
		gzip -d -c $(APR).tar.gz | tar xvf - ;\
		(cd $(APR) && ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	(cd $(APR) && $(MAKE) all install >> make.out)

mkdbgen:
	@echo     -- dbgen
	(cd tpch && $(MAKE) >> make.out)
	(cd tpch && cp -f dbgen ../../build/bin/)
	(cd tpch && cp -f dists.dss ../../build/bin/)

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

