GPCC=$(CURDIR)
APR=apr-1.2.8
APU=apr-util-1.2.8
JSON=json-c-0.6
INCLUDES=-I$(GPCC)/ext/include $$($(GPCC)/ext/bin/apr-1-config --includes) -I$(GPCC)/src/include
CPPFLAGS=$$($(GPCC)/ext/bin/apr-1-config --cppflags)
CFLAGS=-g -Wall $$($(GPCC)/ext/bin/apr-1-config --cflags)
APRLIBS=$$($(GPCC)/ext/bin/apr-1-config --link-ld --libs) 
APULIBS=$$($(GPCC)/ext/bin/apu-1-config --link-ld --libs)


all: mkext mklibgpcc mkgpma mkgp

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


mkext:
	@echo --- ext ------------------------------------------------------
	@(cd ext; mkdir -p lib include)
	@(cd ext; ln -fs $(JSON) json)
	@if ! [ -a ext/json/Makefile ]; then \
		(cd ext; tar xvfz $(JSON).tar.gz) ;\
		(cd ext/json; configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd ext/json; make all; make install >> out)
	@(cd ext; ln -fs $(APR) apr)
	@if ! [ -a ext/apr/Makefile ]; then \
		(cd ext; tar xvfz $(APR).tar.gz) ;\
		(cd ext/apr; configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd ext/apr; make all; make install >> out)
	@(cd ext; ln -fs $(APU) apu)
	@if ! [ -a ext/apu/Makefile ]; then \
		(cd ext; tar xvfz $(APU).tar.gz) ;\
		(cd ext/apu; configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--with-apr=../apr \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd ext/apu; make all; make install >> out)

clean: 
	@rm -rf ext/lib ext/bin ext/include
	@rm -rf ext/$(APR) ext/$(APU) ext/$(JSON)
	@rm -rf src/build
	@find src -name '*.o' -exec rm -f {} \;
