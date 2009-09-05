#-------------------------------------------------------------------------
#
# Makefile for the bootstrap module
#
# $PostgreSQL: pgsql/src/backend/bootstrap/Makefile,v 1.34 2006/03/07 01:03:12 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/bootstrap
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)

OBJS= bootparse.o bootstrap.o 

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^


# bootscanner is compiled as part of bootparse
bootparse.o: $(srcdir)/bootscanner.c

$(srcdir)/bootparse.c: bootparse.y
ifdef BISON
	$(BISON) $(BISONFLAGS) -o $@ $<
else
	@$(missing) bison $< $@
endif

$(srcdir)/bootscanner.c: bootscanner.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif



# bootparse.c and bootscanner.c are in the distribution tarball, so
# they are not cleaned here.
clean:
	rm -f SUBSYS.o $(OBJS)
# And the garbage that might have been left behind by partial build:
	@rm -f y.tab.h y.tab.c y.output lex.yy.c bootparse.tab.c bootparse.tab.h bootstrap_tokens.h bootstrap.h


# This is unusual:  We actually have to build some of the parts before
# we know what the header file dependencies are.  
dep depend: bootparse.c bootscanner.c bootparse.h
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
