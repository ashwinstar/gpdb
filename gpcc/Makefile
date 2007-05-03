GPCC=$(CURDIR)
OPT=-g -Wall
APR=apr-1.2.8
APU=apr-util-1.2.8
JSON=json-c-0.6
LIBEVENT=libevent-1.3b
INCLUDES=-I$(GPCC)/ext/include $$($(GPCC)/ext/bin/apr-1-config --includes) -I$(GPCC)/src/include
CPPFLAGS=$$($(GPCC)/ext/bin/apr-1-config --cppflags)
CFLAGS=$(OPT) $$($(GPCC)/ext/bin/apr-1-config --cflags)
APRLIBS=$$($(GPCC)/ext/bin/apr-1-config --link-ld --libs) 
APULIBS=$$($(GPCC)/ext/bin/apu-1-config --link-ld --libs)


all: mkext mklibgpcc mkgpfdist 

#mkgpma mkgp 

mkgpfdist: 
	@echo --- gpfdist --------------------------------------------------
	@(cd src/gpfdist; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) \
		-o gpfdist *.c -L../build/lib -lgpcc $(APRLIBS) $(APULIBS) -ljson -levent)

mkgp:
	@echo --- gp -------------------------------------------------------

mkgpma:
	@echo --- gpma -----------------------------------------------------
	@(cd src/gpma; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) \
		-o gpma *.c -L../build/lib -lgpcc $(APRLIBS) $(APULIBS) -ljson)

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
		-o net1 net1.c -L../../build/lib -lgpcc $(APRLIBS) $(APULIBS) -ljson)


mkext:
	@echo --- ext ------------------------------------------------------
	@(cd ext; mkdir -p lib include)
	@echo     -- libevent
	@(cd ext; ln -fs $(LIBEVENT) libevent)
	@if ! [ -a ext/libevent/Makefile ]; then \
		(cd ext; tar xvfz $(LIBEVENT).tar.gz) ;\
		(cd ext/libevent; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd ext/libevent; make all install >> out)
	@echo     -- json
	@(cd ext; ln -fs $(JSON) json)
	@if ! [ -a ext/json/Makefile ]; then \
		(cd ext; tar xvfz $(JSON).tar.gz) ;\
		(cd ext/json; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd ext/json; make all; make install >> out)
	@echo     -- apr
	@(cd ext; ln -fs $(APR) apr)
	@if ! [ -a ext/apr/Makefile ]; then \
		(cd ext; tar xvfz $(APR).tar.gz) ;\
		(cd ext/apr; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd ext/apr; make all install >> out)
	@echo     -- apu
	@(cd ext; ln -fs $(APU) apu)
	@if ! [ -a ext/apu/Makefile ]; then \
		(cd ext; tar xvfz $(APU).tar.gz) ;\
		(cd ext/apu; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--with-apr=../apr \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd ext/apu; make all install >> out)

clean: 
	@rm -rf ext/lib ext/bin ext/include
	@rm -rf ext/$(APR) ext/$(APU) ext/$(JSON) ext/$(LIBEVENT)
	@rm -rf src/build
	@find src -name '*.o' -exec rm -f {} \;
