#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for cdb (CDB2)
#
# IDENTIFICATION
#    $Header$
#
#-------------------------------------------------------------------------


subdir = src/backend/cdb
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS := motion
SUBDIROBJS := $(SUBDIRS:%=%/SUBSYS.o)

OBJS = cdbvars.o cdbplan.o cdbllize.o
ALLOBJS = $(OBJS) $(SUBDIROBJS)

all: SUBSYS.o

SUBSYS.o: $(ALLOBJS) 
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $^

$(SUBDIROBJS): $(SUBDIRS:%=%-recursive) ;

.PHONY: $(SUBDIRS:%=%-recursive)
$(SUBDIRS:%=%-recursive): 
	$(MAKE) -C $(subst -recursive,,$@) SUBSYS.o
        
depend dep:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@ || exit; done
	$(CC) -MM $(CFLAGS) *.c >depend

clean:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@ || exit; done
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif
