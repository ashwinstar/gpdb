SHELL=bash
CC=gcc
GPCC=$(CURDIR)
OPT=-g -O3
APR=apr-1.2.12
APU=apr-util-1.2.12
JSON=json-c-0.6
LIBEVENT=libevent-1.3e
INCLUDES=-I$(GPCC)/ext/include $$($(GPCC)/ext/bin/apr-1-config --includes) -I$(GPCC)/src/include -I. $$($(GPCC)/ext/bin/apu-1-config --includes)
CFLAGS=$(OPT) $$($(GPCC)/ext/bin/apr-1-config --cflags) -Wall
APRLIBS=$$($(GPCC)/ext/bin/apr-1-config --link-ld --libs) 
APULIBS= $$($(GPCC)/ext/bin/apu-1-config --link-ld --libs)
GP_VERSION=$$(< $(GPCC)/../VERSION)
CPPFLAGS=$$($(GPCC)/ext/bin/apr-1-config --cppflags) -DGP_VERSION="$(GP_VERSION)"


#all: mkext mkgpfdist mklibgpcc mkgpha mkfts
all: mkdir mkext mkgpfdist mklibgpcc mkgpma mkgpha mkgp mklibchubby mkchubbytest mklibzookeeper mkzookeepertest

mkgpfdist: 
	@echo --- gpfdist --------------------------------------------------
	(cd src/gpfdist; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) \
		-o gpfdist *.c $(APULIBS) $(APRLIBS) -ljson -levent -lz)
	@mkdir -p build/bin
	@cp -f src/gpfdist/gpfdist build/bin/

mklibchubby: src/build/lib/libchubby.a

src/build/lib/libchubby.a: src/paxos/chubby/c/*.[ch]
	@echo --- libchubby --------------------------------------------------
	(cd src/paxos/chubby/c; $(CC) -c -I../include $(INCLUDES) $(CPPFLAGS) $(CFLAGS) -Wall -Werror *.c)
	@echo copying include files
	@(cp -f -r src/paxos/chubby/c/chubby.h src/build/include/ )
	@echo creating library
	@(cd src/paxos/chubby/c; rm -f ../../../build/lib/libchubby.a; \
		ar cru ../../../build/lib/libchubby.a *.o;	\
		ranlib ../../../build/lib/libchubby.a)

mklibzookeeper: src/build/lib/libzookeeper.a

src/build/lib/libzookeeper.a: src/paxos/zookeeper/include/*.h src/paxos/zookeeper/src/*.[ch] src/build/lib/libchubby.a
	@echo --- libzookeeper --------------------------------------------------
	(cd src/paxos/zookeeper/src; $(CC) -c -I../include -I../../../build/include $(INCLUDES) $(CPPFLAGS) $(CFLAGS) -Wall -Werror *.c)
	@echo copying include files
	@(cp -f -r src/paxos/zookeeper/include/* src/build/include/ )
	@echo creating library
	@(cd src/paxos/zookeeper/src; rm -f ../../../build/lib/libzookeeper.a; \
		ar cru ../../../build/lib/libzookeeper.a *.o;	\
		ranlib ../../../build/lib/libzookeeper.a)

mkchubbytest: src/paxos/test/chubbytest

src/paxos/test/chubbytest: src/paxos/test/chubbytest.c src/build/lib/libchubby.a
	@echo --- chubbytest --------------------------------------------------
	(cd src/paxos/test; $(CC) -I../../build/include -L../../build/lib $(INCLUDES) $(CPPFLAGS) $(CFLAGS) -Wall -Werror -o chubbytest chubbytest.c $(APULIBS) $(APRLIBS) -lchubby)

mkzookeepertest: src/paxos/test/zookeepertest

src/paxos/test/zookeepertest: src/paxos/test/zookeepertest.c src/build/lib/libzookeeper.a src/build/lib/libchubby.a
	@echo --- zookeepertest --------------------------------------------------
	(cd src/paxos/test; $(CC) -I../../build/include -L../../build/lib $(INCLUDES) $(CPPFLAGS) $(CFLAGS) -Wall -Werror -o zookeepertest zookeepertest.c $(APULIBS) $(APRLIBS) -lzookeeper -lchubby)

mkgp:
	@echo --- gp -------------------------------------------------------
	@mkdir -p build/bin
	@cp -f src/gp/gp build/bin/

mkgpma:
	@echo --- gpma -----------------------------------------------------
	@(cd src/gpma; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) \
		-o gpma *.c -L../build/lib -lgpcc $(APULIBS) $(APRLIBS) -levent -ljson -lresolv)
	@mkdir -p build/bin
	@cp -f src/gpma/gpma build/bin/


mkgpha:
	@echo --- gpha -----------------------------------------------------
	@(cd src/gpha; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) \
		-o gpha *.c -L../build/lib -lgpcc $(APULIBS) $(APRLIBS) -levent -ljson -lresolv)
	@mkdir -p build/bin
	@cp -f src/gpha/gpha build/bin/

mkfts:
	@echo --- fts -----------------------------------------------------
	@(cd src/fts; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) \
		-o fts *.c -L../build/lib -lgpcc $(APULIBS) $(APRLIBS) -levent -ljson -lresolv)
#	@(cd src/fts; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) \
#		-c *.c)

mkdir:
	@mkdir -p src/build/lib src/build/include build/bin

mklibgpcc: mkdir
	@echo --- libgpcc --------------------------------------------------
	@echo compiling
	@(cd src/libgpcc; $(CC) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) -c *.c)
	@echo creating library
	@(cd src/libgpcc; rm -f ../build/lib/libgpcc.a; \
		ar cru ../build/lib/libgpcc.a *.o;	\
		ranlib ../build/lib/libgpcc.a)
	@echo copying include files
	@(cp -f -r src/include/* src/build/include/ )


mktest:
	@echo --- test ------------------------------------------------------	
	(cd test; $(CC) $(INCLUDES) $(CPPFLAS) $(CFLAGS) \
			-o net1 net1.c -L../src/build/lib -lgpcc $(APULIBS) $(APRLIBS) -ljson -levent -lresolv)

mkext: mkdir
	@echo --- ext ------------------------------------------------------
	@mkdir -p ext/lib ext/include
	@echo     -- libevent
	@(cd ext; rm -f libevent; ln -s $(LIBEVENT) libevent)
	@if ! [ -a ext/libevent/Makefile ]; then \
		(cd ext; gzip -d -c $(LIBEVENT).tar.gz | tar xvf -) ;\
		(cd ext/libevent; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd ext/libevent; $(MAKE) install >> out)
	@echo     -- json
	@(cd ext; rm -f json; ln -s $(JSON) json)
	@if ! [ -a ext/json/Makefile ]; then \
		(cd ext; gzip -d -c $(JSON).tar.gz | tar xvf -) ;\
		(cd ext/json; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd ext/json; $(MAKE) all; $(MAKE) install >> out)
	@echo     -- apr
	@(cd ext; rm -f apr; ln -s $(APR) apr)
	@if ! [ -a ext/apr/Makefile ]; then \
		(cd ext; gzip -d -c $(APR).tar.gz | tar xvf -) ;\
		(cd ext/apr; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd ext/apr; $(MAKE) all install >> out)
	@echo     -- apu
	@(cd ext; rm -f apu; ln -s $(APU) apu)
	@if ! [ -a ext/apu/Makefile ]; then \
		(cd ext; gzip -d -c $(APU).tar.gz | tar xvf -) ;\
		(cd ext/apu; ./configure --enable-shared=no \
			--enable-static=yes --enable-threads \
			--with-apr=../apr \
			--prefix=$(GPCC)/ext) ;\
	fi
	@(cd ext/apu; $(MAKE) all install >> out)

	@echo     -- dbgen
	@(cd ext/tpch; $(MAKE) >> out)
	@(cd ext/tpch; cp -f dbgen ../../build/bin/)
	@(cd ext/tpch; cp -f dists.dss ../../build/bin/)

veryclean: clean
	(cd ext; rm -rf apr apr-1.2.8 apu apr-util-1.2.8 json json-c-0.6 libevent libevent-1.3b-1)

clean: 
	(cd ext; rm -rf lib bin include build-1 share apr apu json libevent)
	@if [ -a ext/apr/Makefile ]; then $(MAKE) clean; fi
	@if [ -a ext/apu/Makefile ]; then $(MAKE) clean; fi
	@if [ -a ext/json/Makefile ]; then $(MAKE) clean; fi
	@if [ -a ext/libevent/Makefile ]; then $(MAKE) clean; fi 
	rm -rf src/build
	find src -name '*.o' -exec rm -f {} \;
