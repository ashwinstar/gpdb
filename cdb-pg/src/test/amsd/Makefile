#
# Makefile for CDB unit tests
#

subdir = src/test/amsd
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -DFRONTEND -I$(libpq_srcdir) $(CPPFLAGS)
override LDLIBS := $(libpq) -lpgport $(LDLIBS)

OBJS = test-amsd.o circbuff.o shmutils.o tupchunk.o
PROGS = test-amsd

all: $(PROGS)

test-amsd: $(OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LIBS) -o $@$(X)

tupchunk.c: $(top_srcdir)/src/backend/cdb/motion/tupchunk.c
	rm -f $@ && $(LN_S) $<

shmutils.c: $(top_srcdir)/src/backend/cdb/motion/shmutils.c
	rm -f $@ && $(LN_S) $<

circbuff.c: $(top_srcdir)/src/backend/cdb/motion/circbuff.c
	rm -f $@ && $(LN_S) $<

clean:
	rm -f $(PROGS) $(OBJS)
