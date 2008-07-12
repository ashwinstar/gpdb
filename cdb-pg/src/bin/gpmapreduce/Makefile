
TARGET     = mapred
LIBYAML    = libyaml.a
LIBYAMLDIR = yaml-0.1.1

CC        = gcc
LD        = gcc
YACC      = bison
FLEX      = flex
FIXUP     = ./bin/fixup.pl

SRCDIR    = src
INCDIR    = include
LIBDIR    = lib
TESTDIR   = test

PQINCDIR  = $(shell pg_config --includedir)
PGINCDIR  = $(shell pg_config --includedir-server)
PGLIBDIR  = $(shell pg_config --libdir)

CFLAGS    = -I$(PQINCDIR) -I$(PGINCDIR) -I$(INCDIR) -I$(SRCDIR) -g
LDFLAGS   = -L$(LIBDIR) -lyaml -L$(PGLIBDIR) -lpq
YACCFLAGS = -d
FLEXFLAGS = -i

FILES     = main.o mapred.o parse.o yaml_scan.o yaml_parse.o 
GENFILES  = yaml_parse.h yaml_parse.c yaml_scan.c yaml_scan.h

OBJECTS   = $(addprefix $(SRCDIR)/, $(FILES))
GENOBJS   = $(addprefix $(SRCDIR)/, $(GENFILES))

all: $(TARGET)

check: $(TARGET)
	@cd $(TESTDIR); make

clean:
	make test_clean
	rm -rf $(GENOBJS)
	rm -rf $(TARGET) $(SRCDIR)/*.o

test_clean:
	@cd $(TESTDIR); make clean >/dev/null

$(TARGET): $(LIBDIR)/$(LIBYAML) $(GENOBJS) $(OBJECTS)
	@echo "[Linking]  $(TARGET)"
	@$(LD) $(OBJECTS) $(LDFLAGS) -o $(TARGET)
	@echo 

$(LIBDIR)/$(LIBYAML):
	@cp $(LIBYAMLDIR)/include/yaml.h $(INCDIR)/.
	@chmod u+w $(INCDIR)/yaml.h
	@cd $(LIBYAMLDIR); configure
	@cd $(LIBYAMLDIR); make
	@cp $(LIBYAMLDIR)/src/.libs/$(LIBYAML) $(LIBDIR)/$(LIBYAML)
	@chmod u+w $(LIBDIR)/$(LIBYAML)

%.o: %.c 
	@echo "[Building] $@"
	@$(CC) $(CFLAGS) -c $*.c -o $@

# This is a bit redundant, but it works well enough
$(SRCDIR)/yaml_scan.c : $(SRCDIR)/yaml_scan.l
	@echo "[Flex]     $*.l"
	@cd $(SRCDIR) ; \
	$(FLEX) $(FLEXFLAGS) yaml_scan.l

$(SRCDIR)/yaml_scan.h : $(SRCDIR)/yaml_scan.l
	@echo "[Flex]     $*.l"
	@cd $(SRCDIR) ; \
	$(FLEX) $(FLEXFLAGS) yaml_scan.l

$(SRCDIR)/yaml_parse.c: $(SRCDIR)/yaml_parse.y
	@echo "[Bison]    $*.y"
	@cd $(SRCDIR) ; \
	$(YACC) $(YACCFLAGS) yaml_parse.y

$(SRCDIR)/yaml_parse.h : $(SRCDIR)/yaml_parse.y
	@echo "[Bison]    $*.y"
	@cd $(SRCDIR) ; \
	$(YACC) $(YACCFLAGS) yaml_parse.y

# Other dependencies
$(SRCDIR)/yaml_parse.c  : $(SRCDIR)/yaml_scan.h
$(SRCDIR)/yaml_parse.c  : $(INCDIR)/parser.h $(INCDIR)/mapred.h
$(SRCDIR)/yaml_scan.c   : $(INCDIR)/parser.h $(INCDIR)/mapred.h
$(SRCDIR)/parse.o       : $(SRCDIR)/yaml_parse.h
$(SRCDIR)/parse.o       : $(INCDIR)/parser.h $(INCDIR)/mapred.h
$(SRCDIR)/mapred.o      : $(INCDIR)/parser.h $(INCDIR)/mapred.h
$(SRCDIR)/main.o        : $(INCDIR)/mapred.h
