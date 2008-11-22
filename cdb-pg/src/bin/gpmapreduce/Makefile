
subdir = src/bin/gpmapreduce
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifdef srcdir
INCDIR= $(srcdir)/include
SRCDIR= $(srcdir)/src
else
INCDIR= include
SRCDIR= src
endif

override CPPFLAGS += -DFRONTEND -I$(SRCDIR) -I$(INCDIR) -I$(libpq_srcdir)
override LDFLAGS  += -lyaml

FILES     = main.o mapred.o parse.o yaml_scan.o yaml_parse.o 
GENFILES  = yaml_parse.h yaml_parse.c yaml_scan.c yaml_scan.h

OBJS      = $(addprefix $(SRCDIR)/, $(FILES))
GENOBJS   = $(addprefix $(SRCDIR)/, $(GENFILES))

FLEXFLAGS = -i
YACCFLAGS = -d

all: submake-libpq gpmapreduce

gpmapreduce: $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(OBJS) $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)

internal:
	rm $(SRCDIR)/mapred.o
	$(MAKE) CFLAGS="-DINTERNAL_BUILD" all

check:
	$(MAKE) -C test

$(srcdir)/yaml_scan.c: yaml_scan.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif

$(SRCDIR)/yaml_scan.c: $(SRCDIR)/yaml_scan.l
ifdef FLEX
	cd $(SRCDIR) ; \
	$(FLEX) $(FLEXFLAGS) yaml_scan.l
else
	@$(missing) flex $< $@
endif

$(SRCDIR)/yaml_parse.h: $(SRCDIR)/yaml_parse.c
$(SRCDIR)/yaml_parse.c: $(SRCDIR)/yaml_parse.y
ifdef YACC
	@cd $(SRCDIR) ; \
	$(YACC) $(YACCFLAGS) yaml_parse.y
else
	@$(missing) bison $< $@
endif

install: all installdirs
	$(INSTALL_PROGRAM) gpmapreduce$(X) '$(DESTDIR)$(bindir)/gpmapreduce$(X)'

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/gpmapreduce$(X)'

clean distclean maintainer-clean:
	rm -f gpmapreduce$(X) $(OBJS) $(GENOBJS)

# Other dependencies
$(SRCDIR)/yaml_parse.c  : $(INCDIR)/parser.h $(INCDIR)/mapred.h
$(SRCDIR)/yaml_scan.c   : $(INCDIR)/parser.h $(INCDIR)/mapred.h
$(SRCDIR)/parse.o       : $(SRCDIR)/yaml_parse.h
$(SRCDIR)/parse.o       : $(INCDIR)/parser.h $(INCDIR)/mapred.h
$(SRCDIR)/mapred.o      : $(INCDIR)/parser.h $(INCDIR)/mapred.h
$(SRCDIR)/main.o        : $(INCDIR)/mapred.h

