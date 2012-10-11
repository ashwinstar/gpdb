subdir=src/backend/utils/misc/fstream
top_builddir=../../../../../..

TARGETS=fstream

# Objects from backend, which don't need to be mocked but need to be linked.
$(TARGETS)_REAL_OBJS=\
	$(top_srcdir)/src/backend/bootstrap/bootparse.o \
	$(top_srcdir)/src/backend/nodes/makefuncs.o \
	$(top_srcdir)/src/backend/parser/gram.o \
	$(top_srcdir)/src/backend/utils/adt/like.o \
	$(top_srcdir)/src/backend/utils/misc/guc.o \
	$(top_srcdir)/src/backend/nodes/value.o \
	$(top_srcdir)/src/backend/nodes/list.o \
	$(top_srcdir)/src/timezone/localtime.o \
	$(top_srcdir)/src/timezone/pgtz.o \
	$(top_srcdir)/src/port/memmem.o

include $(top_builddir)/src/Makefile.mock
