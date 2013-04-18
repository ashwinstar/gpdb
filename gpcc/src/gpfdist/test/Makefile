subdir=gpcc/src/gpfdist/test
top_builddir=../../../../cdb-pg

include $(top_builddir)/src/Makefile.global

override CPPFLAGS+= -I$(top_builddir)/src/test/unit/cmockery $(top_builddir)/src/test/unit/cmockery/cmockery.o -I../

TARGETS=gpfdist_test

all: $(TARGETS)

.PHONY:
check: $(patsubst %,%-check,$(TARGETS))

.PHONY:
%-check: all
	./$*

.PHONY:
clean: $(patsubst %,%-clean,$(TARGETS))

.PHONY:
%-clean:
	rm -f $*

