BLD_TOP=../../../..
include $(BLD_TOP)/Makefile.global
CC=$(strip $(BLD_CC) $(BLD_CFLAGS))

top_builddir=../../..

CMOCKERY_DIR=$(top_builddir)/src/test/unit/cmockery
CMOCKERY_OBJS=$(CMOCKERY_DIR)/cmockery.o
override CFLAGS+= -I$(CMOCKERY_DIR) -I$(top_builddir)/src/include

TARGETS=memmem

# *_test.o files are intermediate files.
# Mark them as secondary in order not to be deleted automatically.
.SECONDARY:
%_test.o: %_test.c
	$(CC) $(CFLAGS) -c $< -o $@

%.t: %_test.o
	$(CC) $^ $(CMOCKERY_OBJS) -o $@

%-exec-test: %.t
	./$^

.PHONY:
check: $(patsubst %,%-exec-test,$(TARGETS))

.PHONY:
unittest-check: check

.PHONY:
clean:
	rm -f *.t
	rm -f *.o
