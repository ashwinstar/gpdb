SHELL=bash
CC=gcc
GPCC=$(CURDIR)
OPT=-O3
APR=apr-1.2.8
APU=apr-util-1.2.8
JSON=json-c-0.6
LIBEVENT=libevent-1.3b-1
INCLUDES=-I$(GPCC)/ext/include $$($(GPCC)/ext/apr/apr-1-config --includes) $$($(GPCC)/ext/apu/apu-1-config --includes) -I$(GPCC)/ext/$(LIBEVENT) -I$(GPCC)/ext -I$(GPCC)/ext/$(JSON) -I$(GPCC)/src/include
CFLAGS=$(OPT) $$($(GPCC)/ext/apr/apr-1-config --cflags)
APRLIBS=-L$(GPCC)/ext/apr/.libs $$($(GPCC)/ext/apr/apr-1-config --link-ld --libs) 
APULIBS=-L$(GPCC)/ext/apu/.libs -L$(GPCC)/ext/apu/xml/expat/lib/.libs -laprutil-1 -lexpat
GP_VERSION=$$(< $(GPCC)/../VERSION)
CPPFLAGS=$$($(GPCC)/ext/apr/apr-1-config --cppflags) -DGP_VERSION="$(GP_VERSION)"


all: mkext mklibgpcc mkgpfdist mkgpma

#mkgpma mkgp 

mkgpfdist: 
	@echo --- gpfdist --------------------------------------------------
	(cd src/gpfdist; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) \
		-o gpfdist *.c -L../build/lib -lgpcc $(APRLIBS) $(APULIBS) -L$(GPCC)/ext/$(JSON)/.libs -L$(GPCC)/ext/$(LIBEVENT)/.libs -ljson -levent)
	@mkdir -p build/bin
	@cp src/gpfdist/gpfdist build/bin/

mkgp:
	@echo --- gp -------------------------------------------------------

mkgpma:
	@echo --- gpma -----------------------------------------------------
	@(cd src/gpma; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) \
		-o gpma *.c -L../build/lib -lgpcc $(APRLIBS) $(APULIBS) -L$(GPCC)/ext/$(JSON)/.libs -ljson)

mklibgpcc:
	@echo --- libgpcc --------------------------------------------------
	@mkdir -p src/build/lib src/build/include
	@echo compiling
	@(cd src/libgpcc; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) -c *.c)
	@echo creating library
	@(cd src/libgpcc; rm -f ../build/lib/libgpcc.a; \
		ar cru ../build/lib/libgpcc.a *.o;	\
		ranlib ../build/lib/libgpcc.a)
	@echo copying include files
	@(cp -r src/include/* src/build/include/ )
	#@echo making test
	#@(cd src/libgpcc/test; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) \
	#	-o net1 net1.c -L../../build/lib -lgpcc $(APRLIBS) $(APULIBS) -ljson)


mkext:
	@echo --- ext ------------------------------------------------------
	@(cd ext; mkdir -p lib include)
	@echo     -- libevent
	@(cd ext; rm -f libevent; ln -s $(LIBEVENT) libevent)
	@if ! [ -a ext/libevent/Makefile ]; then \
		(cd ext; gzip -d -c $(LIBEVENT).tar.gz | tar xvf -) ;\
		(cd ext/libevent; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd ext/libevent; $(MAKE) >> out)
	@echo     -- json
	@(cd ext; rm -f json; ln -s $(JSON) json)
	@if ! [ -a ext/json/Makefile ]; then \
		(cd ext; gzip -d -c $(JSON).tar.gz | tar xvf -) ;\
		(cd ext/json; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd ext/json; $(MAKE) all >> out)
	@echo     -- apr
	@(cd ext; rm -f apr; ln -s $(APR) apr)
	@if ! [ -a ext/apr/Makefile ]; then \
		(cd ext; gzip -d -c $(APR).tar.gz | tar xvf -) ;\
		(cd ext/apr; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd ext/apr; $(MAKE) all >> out)
	@echo     -- apu
	@(cd ext; rm -f apu; ln -s $(APU) apu)
	@if ! [ -a ext/apu/Makefile ]; then \
		(cd ext; gzip -d -c $(APU).tar.gz | tar xvf -) ;\
		(cd ext/apu; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--with-apr=../apr \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd ext/apu; $(MAKE) all >> out)

clean: 
	(cd ext; rm -rf lib bin include build-1 share apr apu json libevent)
	(cd ext; rm -rf $(APR) $(APU) $(JSON) $(LIBEVENT))
	rm -rf src/build
	find src -name '*.o' -exec rm -f {} \;
