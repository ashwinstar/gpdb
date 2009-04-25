# prevent included makefiles from stealing default target
default: all

BLD_TOP=$(shell dirname `pwd`)
include $(BLD_TOP)/Makefile.global
include $(BLD_TOP)/Makefile.thirdparty

# NO_M64 set in Makefile.global turns off default to 64-bit
# here, select bitness we desire this build for gcc and ld
CC=$(strip gcc $(BLD_CFLAGS))

GPCC=$(CURDIR)
CDBINCL=-I$(GPCC)/../cdb-pg/src/include
GPFDISTFILES=fstream.c gfile.c gpfdist.c

ifeq "$(BLDWIN)" "yes"
override CC=$(strip /usr/local/mingw32/bin/mingw32-gcc $(BLD_CFLAGS))
override BLD_CFLAGS += -L/usr/local/mingw32/mingw32/lib -I/usr/local/mingw32/mingw32/include -DWIN32 $(CDBINCL)/port/win32 $(CDBINCL)/port/win32_msvc -DFRONTEND
override GPFDISTFILES += glob.c $(CURDIR)/../cdb-pg/src/port/dirent.c $(CURDIR)/../cdb-pg/src/port/strlcpy.c $(CURDIR)/../cdb-pg/src/port/win32error.c $(CURDIR)/../cdb-pg/src/port/snprintf.c
GPFDIST_INCLUDES=$(GPCC)/src/gpfdist/include $(GPCC)
GPFDIST_LIBS=-lws2_32
else
GPFDIST_LIBS=-lz -lbz2
endif

export CC
export LDFLAGS=$(BLD_CFLAGS)

SHELL=bash
OPT=-g -O3
LIBEVENT=libevent-1.4.3-stable
OPENSSL=openssl-0.9.8h
GP_VERSION=$$(< $(GPCC)/../VERSION)

ifeq "no-match$(findstring $(BLD_ARCH),osx105_x86 rhel4_x86_32 rhel5_x86_64 rhel5_x86_32 suse10_x86_64)" "no-match"
INCLUDES=-I$(BLD_THIRDPARTY_INCLUDE_DIR) -I$(GPCC)/ext/include $$($(GPCC)/ext/bin/apr-1-config --includes) -I$(GPCC)/src/include -I. $$($(GPCC)/ext/bin/apu-1-config --includes)
CFLAGS=$(OPT) $$($(GPCC)/ext/bin/apr-1-config --cflags) -Wall
APRLIBS=$$($(GPCC)/ext/bin/apr-1-config --link-ld --libs) 
APULIBS= $$($(GPCC)/ext/bin/apu-1-config --link-ld --libs)
CPPFLAGS=$$($(GPCC)/ext/bin/apr-1-config --cppflags) -DGP_VERSION="$(GP_VERSION)"
else
INCLUDES=-I$(BLD_THIRDPARTY_INCLUDE_DIR) -I$(GPCC)/ext/include $$($(BLD_THIRDPARTY_BIN_DIR)/apr-1-config --includes) -I$(GPCC)/src/include -I. $$($(BLD_THIRDPARTY_BIN_DIR)/apu-1-config --includes)
CFLAGS=$(OPT) $$($(BLD_THIRDPARTY_BIN_DIR)/apr-1-config --cflags) -Wall
APRLIBS=$$($(BLD_THIRDPARTY_BIN_DIR)/apr-1-config --link-ld --libs) 
APULIBS= $$($(BLD_THIRDPARTY_BIN_DIR)/apu-1-config --link-ld --libs)
CPPFLAGS_GPFDIST=$$($(BLD_THIRDPARTY_BIN_DIR)/apr-1-config --cppflags)
CPPFLAGS=$$($(BLD_THIRDPARTY_BIN_DIR)/apr-1-config --cppflags) -DGP_VERSION="$(GP_VERSION)"
endif

osx105_x86_GPFDIST_LDFLAGS=-Wl,-search_paths_first
GPFDIST_LDFLAGS=$($(BLD_ARCH)_GPFDIST_LDFLAGS)

#all: mkext mkgpfdist mklibgpcc mkgpha mkfts
#all: mkdir mkext mkgpfdist mklibgpcc mkgp mklibchubby mkchubbytest mkpaxos mkgpha mkgpma
all: mkdir mkgpfdist

#mkgpma mkgp 

mkpaxos: mkdir
	(cd src/paxos;sh build)
	mkdir -p build/bin/lib/paxos
	cp src/paxos/bin/*.pyc build/bin/lib/paxos
	cp src/paxos/gppaxos build/bin
	chmod 755 build/bin/gppaxos

mkgpfdist: mkext
	@echo --- gpfdist --------------------------------------------------
	cd src/gpfdist/ && \
	  ln -sf ../../../cdb-pg/src/backend/utils/misc/fstream/*.c .
	mkdir -p src/gpfdist/include
	cd src/gpfdist/ && \
	  $(CC) $(INCLUDES) $(CDBINCL) $(CPPFLAGS_GPFDIST) $(CFLAGS) $(GPFDIST_LDFLAGS) -Werror -o gpfdist \
	  $(GPFDISTFILES) $(APULIBS) $(APRLIBS) -ljson -levent -L$(BLD_THIRDPARTY_LIB_DIR) $(GPFDIST_LIBS)
	mkdir -p build/bin
	cp -f src/gpfdist/gpfdist build/bin/

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

mkext: mkdir
	$(MAKE) -C ext

veryclean: clean
	$(MAKE) -C ext veryclean

clean: 
	$(MAKE) -C ext veryclean
	rm -f src/gpfdist/gpfdist
	rm -rf src/build
	find src -name '*.o' -exec rm -f {} \;
