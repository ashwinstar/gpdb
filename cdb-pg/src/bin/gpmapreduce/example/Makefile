YMLDIR = yml
OUTDIR = output
EXPECT = expected
HOSTNAME = $(shell hostname)
DATADIR = $(shell pwd)/data

D       = /usr/bin/diff
MAPRED  = ../bin/mapred.pl
DOCMAP  = ../bin/docmap.pl
PATHFIX = ../bin/fixup.pl
MODDIR  = ../lib/GPMapReduce
MODULES = Document.pm Parser.pm Function.pm Mapper.pm Utils.pm
MAPMODS = $(addprefix $(MODDIR)/, $(MODULES))


TEST   = access_frequency grep sort columns graph wordcount
YML    = $(addsuffix .yml, $(addprefix $(YMLDIR)/, $(TEST)))
OUT    = $(addsuffix .out, $(addprefix $(OUTDIR)/, $(TEST)))
PLAN   = $(addsuffix .plan, $(addprefix $(OUTDIR)/, $(TEST)))
PRINT  = $(addsuffix .print, $(addprefix $(OUTDIR)/, $(TEST)))
EXPRNT  = $(addsuffix .print, $(addprefix $(EXPECT)/, $(TEST)))

OBJS   = $(addprefix $(SRCDIR), $(YML))

.PHONY: clean all

all: $(YML) $(OUT) $(PLAN) $(PRINT) $(EXPRNT)
	-$(D) -q -B -x '*.in' -x '*.in~' -x README $(OUTDIR) $(EXPECT)

$(YMLDIR)/%.yml: $(YMLDIR)/%.yml.in
	$(PATHFIX) $@.in

$(EXPECT)/%.print: $(EXPECT)/%.print.in
	$(PATHFIX) $@.in

$(OUTDIR)/%.out: $(MAPRED) $(MAPMODS) $(DOCMAP) $(YMLDIR)/%.yml 
	$(MAPRED) $(YMLDIR)/$*.yml admin > $@

$(OUTDIR)/%.plan: $(MAPRED) $(MAPMODS) $(DOCMAP) $(YMLDIR)/%.yml
	$(MAPRED) --explain $(YMLDIR)/$*.yml admin > $@

$(OUTDIR)/%.print: $(MAPRED) $(MAPMODS) $(DOCMAP) $(YMLDIR)/%.yml 
	$(MAPRED) --print $(YMLDIR)/$*.yml admin > $@

$(YMLDIR)/wordcount.yml: $(DATADIR)/email/mailfiles

$(DATADIR)/email/mailfiles:
	ls -1 $(DATADIR)/email/*.txt > $@

clean:
	rm -f $(YML) 
	rm -f $(EXPRNT) 
	rm -f $(OUTDIR)/*

