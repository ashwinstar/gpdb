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

all: 
	@echo --- ext ------------------------------------------------------
	@echo $(GPCC)
	@mkdir -p lib include
	@echo     -- libevent
	@rm -f libevent; ln -s $(LIBEVENT) libevent
	@if ! [ -a libevent/Makefile ]; then \
		gzip -d -c $(LIBEVENT).tar.gz | tar xvf - ;\
		(cd libevent; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd libevent; $(MAKE) install >> make.out)
	@echo     -- json
	@rm -f json; ln -s $(JSON) json
	@if ! [ -a json/Makefile ]; then \
		gzip -d -c $(JSON).tar.gz | tar xvf - ;\
		(cd json; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd json; $(MAKE) all; $(MAKE) install >> make.out)
	@echo     -- openssl
	@rm -f openssl; ln -s $(OPENSSL) openssl
	@if ! [ -a openssl/Makefile ]; then \
		gzip -d -c $(OPENSSL).tar.gz | tar xvf - ;\
		(cd openssl; ./config no-shared threads --prefix=$(GPCC)/ext) ;\
	fi
	@(cd openssl; $(MAKE)  all install >> make.out 2>&1)
	@echo     -- apr
	@rm -f apr; ln -s $(APR) apr
	@if ! [ -a apr/Makefile ]; then \
		gzip -d -c $(APR).tar.gz | tar xvf - ;\
		(cd apr; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd apr; $(MAKE) all install >> make.out)
	@echo     -- apu
	@rm -f apu; ln -s $(APU) apu
	@if ! [ -a apu/Makefile ]; then \
		gzip -d -c $(APU).tar.gz | tar xvf - ;\
		(cd apu; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--with-apr=../apr \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd apu; $(MAKE) all install >> make.out)

	@echo     -- dbgen
	@(cd tpch; $(MAKE) >> make.out)
	@(cd tpch; cp -f dbgen ../../build/bin/)
	@(cd tpch; cp -f dists.dss ../../build/bin/)

	(cd libsigar; cp -rf include/* ../include)
	(cd libsigar; $(MAKE); cp -f src/libsigar.a ../lib)


veryclean:
	rm -rf lib bin include build1 share ssl build-1
	rm -rf apr $(APR) apu $(APU) json $(JSON) libevent $(LIBEVENT) openssl $(OPENSSL)
	(cd libsigar; $(MAKE) veryclean)

clean:
	rm -rf lib bin include build1 share ssl build-1
	@(cd apr; $(MAKE) clean; rm -f make.out)
	@(cd apu; $(MAKE) clean; rm -f make.out)
	@(cd json; $(MAKE) clean; rm -f make.out)
	@(cd libevent; $(MAKE) clean; rm -f make.out)
	@(cd openssl; $(MAKE) clean; rm -f make.out)
	@(cd libsigar; $(MAKE) clean)

