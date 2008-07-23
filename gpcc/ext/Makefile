SHELL=bash
CC=gcc
GPCC=$(shell echo `pwd`/..)
CDBINCL=-I$(GPCC)/../cdb-pg/src/include
OPT=-g -O3
APR=apr-1.2.12
APU=apr-util-1.2.12
JSON=json-c-0.6
LIBEVENT=libevent-1.4.4-stable


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
	@(cd libevent; $(MAKE) install >> out)
	@echo     -- json
	@rm -f json; ln -s $(JSON) json
	@if ! [ -a json/Makefile ]; then \
		gzip -d -c $(JSON).tar.gz | tar xvf - ;\
		(cd json; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd json; $(MAKE) all; $(MAKE) install >> out)
	@echo     -- apr
	@rm -f apr; ln -s $(APR) apr
	@if ! [ -a apr/Makefile ]; then \
		gzip -d -c $(APR).tar.gz | tar xvf - ;\
		(cd apr; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd apr; $(MAKE) all install >> out)
	@echo     -- apu
	@rm -f apu; ln -s $(APU) apu
	@if ! [ -a apu/Makefile ]; then \
		gzip -d -c $(APU).tar.gz | tar xvf - ;\
		(cd apu; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--with-apr=../apr \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd apu; $(MAKE) all install >> out)

	@echo     -- dbgen
	@(cd tpch; $(MAKE) >> out)
	@(cd tpch; cp -f dbgen ../../build/bin/)
	@(cd tpch; cp -f dists.dss ../../build/bin/)

	@echo     -- libsigar
	(cd libsigar; cp -rf include/* ../include)
	(cd libsigar; $(MAKE); cp -f src/libsigar.a ../lib)


veryclean: clean
	rm -rf apr $(APR) apu $(APU) json $(JSON) libevent $(LIBEVENT)
	(cd libsigar; $(MAKE) veryclean)

clean:
	rm -rf lib bin include build1 share
	@(cd apr; $(MAKE) clean)
	@(cd apu; $(MAKE) clean)
	@(cd json; $(MAKE) clean)
	@(cd libevent; $(MAKE) clean)
	@(cd libsigar; $(MAKE) clean)
