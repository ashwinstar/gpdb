BLD_TOP=$(shell echo `pwd`/../../../)
include $(BLD_TOP)/Makefile.thirdparty

EXT_DIR=$(BLD_TOP)/ext/$(BLD_ARCH)/
LIBPQ_DIR=$(BLD_TOP)/cdb-pg/src/interfaces/libpq/

OPT=-g -O3 -Wall

GPPERFMON_VERSION=$$(< $(BLD_TOP)/gpperfmon/VERSION)

GPMMON_OBJS=gpmmon.o gpmondb.o gpmonlib.o gpmon_agg.o
GPSMON_OBJS=gpsmon.o gpmonlib.o

INCS=-I$(EXT_DIR)/include -I$(EXT_DIR)/include/apr-1 -I$(BLD_TOP)/cdb-pg/src/include -I$(LIBPQ_DIR)
LIBS=-L$(EXT_DIR)/lib -lapr-1 -laprutil-1 -levent
GPSMON_LIBS=
GPMMON_LIBS=-L$(LIBPQ_DIR) -lpq
CFLAGS=$(OPT)
CPPFLAGS=$$($(EXT_DIR)/bin/apr-1-config --cppflags) -DGPPERFMON_VERSION="$(GPPERFMON_VERSION)"

ifeq (osx104_x86, $(BLD_ARCH))
	GPSMON_LIBS=-framework CoreServices -framework IOKit
endif

GPSMON_LIBS+= -lm -lsigar

gpsmon: $(GPSMON_OBJS)
	$(CC) -o gpsmon $(GPSMON_OBJS) $(LIBS) $(GPSMON_LIBS)

gpmmon: $(GPMMON_OBJS)
	$(CC) -o gpmmon $(GPMMON_OBJS) $(LIBS) $(GPMMON_LIBS)

%.o: %.c
	$(CC) $(INCS) $(CPPFLAGS) $(CFLAGS) -o $@ -c $<

clean:
	rm -rf *.o gpmmon gpsmon
