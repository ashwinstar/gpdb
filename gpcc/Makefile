# prevent included makefiles from stealing default target
default: all

BLD_TOP=$(shell dirname `pwd`)
include $(BLD_TOP)/Makefile.thirdparty

SHELL=bash
CC=gcc
GPCC=$(CURDIR)
CDBINCL=-I$(GPCC)/../cdb-pg/src/include
OPT=-g -O3
LIBEVENT=libevent-1.4.3-stable
OPENSSL=openssl-0.9.8h
INCLUDES=-I$(BLD_THIRDPARTY_INCLUDE_DIR) -I$(GPCC)/ext/include $$($(GPCC)/ext/bin/apr-1-config --includes) -I$(GPCC)/src/include -I. $$($(GPCC)/ext/bin/apu-1-config --includes)
CFLAGS=$(OPT) $$($(GPCC)/ext/bin/apr-1-config --cflags) -Wall
APRLIBS=$$($(GPCC)/ext/bin/apr-1-config --link-ld --libs) 
APULIBS= $$($(GPCC)/ext/bin/apu-1-config --link-ld --libs)
GP_VERSION=$$(< $(GPCC)/../VERSION)
CPPFLAGS=$$($(GPCC)/ext/bin/apr-1-config --cppflags) -DGP_VERSION="$(GP_VERSION)"

#all: mkext mkgpfdist mklibgpcc mkgpha mkfts
all: mkdir mkext mkgpfdist mklibgpcc mkgp mklibchubby mkchubbytest mkpaxos mkgpha mkgpma

#mkgpma mkgp 

mkpaxos: mkdir
	(cd src/paxos;sh build)
	mkdir -p build/bin/lib/paxos
	cp src/paxos/bin/*.pyc build/bin/lib/paxos
	cp src/paxos/gppaxos build/bin
	chmod 755 build/bin/gppaxos

mkgpfdist: mkext
	@echo --- gpfdist --------------------------------------------------
	(cd src/gpfdist;ln -sf ../../../cdb-pg/src/backend/utils/misc/fstream/*.c .;mkdir -p include;ln -sf ../../../cdb-pg/src/include/fstream fstream; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) \
		-Werror -o gpfdist *.c $(APULIBS) $(APRLIBS) -ljson -levent -L$(BLD_THIRDPARTY_LIB_DIR) -lz -lbz2)
	@mkdir -p build/bin
	@cp -f src/gpfdist/gpfdist build/bin/

mklibchubby: src/build/lib/libchubby.a

src/build/lib/libchubby.a: mkext src/paxos/chubby/c/*.[ch]
	@echo --- libchubby --------------------------------------------------
	(cd src/paxos/chubby/c; $(CC) -c -I../include $(INCLUDES) $(CPPFLAGS) $(CFLAGS) -Wall -Werror *.c)
	@echo copying include files
	@(cp -f -r src/paxos/chubby/c/chubby.h src/build/include/ )
	@echo creating library
	@(cd src/paxos/chubby/c; rm -f ../../../build/lib/libchubby.a; \
		ar cru ../../../build/lib/libchubby.a *.o;	\
		ranlib ../../../build/lib/libchubby.a)

mkchubbytest: src/paxos/test/chubbytest

src/paxos/test/chubbytest: src/paxos/test/chubbytest.c src/build/lib/libchubby.a
	@echo --- chubbytest --------------------------------------------------
	(cd src/paxos/test; $(CC) -I../../build/include -L../../build/lib $(INCLUDES) $(CPPFLAGS) $(CFLAGS) -Wall -Werror -o chubbytest chubbytest.c -lchubby $(APULIBS) $(APRLIBS))

mkgp:
	@echo --- gp -------------------------------------------------------
	@mkdir -p build/bin
	@cp -f src/gp/gp build/bin/

mkgpma: mklibgpcc mklibchubby
	@echo --- gpma -----------------------------------------------------
	@(cd src/gpma; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) -I../build/include \
		-o gpma *.c -L../build/lib -lgpcc -levent -ljson -lresolv -lchubby -lssl -lcrypto $(APULIBS) $(APRLIBS))
	@mkdir -p build/bin
	@cp -f src/gpma/gpma build/bin/


mkgpha: mklibgpcc
	@echo --- gpha -----------------------------------------------------
	@(cd src/gpha; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) \
		-o gpha *.c -L../build/lib -lgpcc $(APULIBS) $(APRLIBS) -levent -ljson -lresolv -lssl -lcrypto)
	@mkdir -p build/bin
	@cp -f src/gpha/gpha build/bin/

mkdir:
	@mkdir -p src/build/lib src/build/include build/bin build/bin/lib

mklibgpcc: mkext
	@echo --- libgpcc --------------------------------------------------
	@echo compiling
	@(cd src/libgpcc; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) -c *.c)
	@echo creating library
	@(cd src/libgpcc; rm -f ../build/lib/libgpcc.a; \
		ar cru ../build/lib/libgpcc.a *.o;	\
		ranlib ../build/lib/libgpcc.a)
	@echo copying include files
	@(cp -f -r src/include/* src/build/include/ )


mktest: mklibgpcc
	@echo --- test ------------------------------------------------------	
	(cd test; $(CC) $(INCLUDES) $(CPPFLAS) $(CFLAGS) \
			-o net1 net1.c -L../src/build/lib -lgpcc $(APULIBS) $(APRLIBS) -ljson -levent -lresolv)

mkext: 
	$(MAKE) -C ext

veryclean: clean
	$(MAKE) -C ext veryclean

clean: 
	$(MAKE) -C ext veryclean
	rm -rf src/build
	find src -name '*.o' -exec rm -f {} \;
