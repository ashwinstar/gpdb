#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/file
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/storage/file/Makefile,v 1.11 2003/11/29 19:51:56 pgsql Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/file
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = fd.o buffile.o bfz.o pipe.o compress_nothing.o compress_zlib.o compress_threaded.o compress_lzop.o

all: SUBSYS.o

LZO_LIBS=

#CFLAGS += -DWITH_LZO
#LZO_LIBS += -llzo2

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS) $(LZO_LIBS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif
