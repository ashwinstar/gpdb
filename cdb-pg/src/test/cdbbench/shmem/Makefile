#
# Makefile for CDB unit tests
#

subdir = src/test/cdbbench/shmem
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
override LDLIBS := $(libpq) -lpgport $(LDLIBS)

OBJS = shmbufftest.o circbuff.o shmutils.o
PROGS = shmbufftest

all: $(PROGS)

shmbufftest: $(OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LIBS) -o $@$(X)


shmutils.c: $(top_srcdir)/src/backend/cdb/motion/shmutils.c
	rm -f $@ && $(LN_S) $<

circbuff.c: $(top_srcdir)/src/backend/cdb/motion/circbuff.c
	rm -f $@ && $(LN_S) $<

clean:
	rm -f $(PROGS) $(OBJS)
