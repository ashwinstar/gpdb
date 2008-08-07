
MAPRED    = ../gpmapreduce
FIXUP     = bin/fixup.pl
MRDIFF    = bin/mrdiff.pl

MFLAGS    = --key="clar"

INDIR     = input
DATADIR   = data
MASTERDIR = expected

OUTDIR    = output
YMLDIR    = yml

YMLIN     = $(shell ls $(INDIR)/*.yml.in)
YML       = $(notdir $(basename $(YMLIN)))
YMLOUT    = $(addprefix $(YMLDIR)/, $(YML))

TESTS     = $(addprefix $(OUTDIR)/,$(addsuffix .suc, $(basename $(YML))))
MAILDATA  = $(addprefix $(shell pwd)/,$(DATADIR)/email/mailfiles)

all: $(YMLDIR) $(OUTDIR) $(YMLOUT) $(MAILDATA) $(TESTS)
	@cd $(OUTDIR); if [ -s FAIL ] ; then cat FAIL ; else echo [SUCCESS] ; fi
	@rm -rf $(OUTDIR)/FAIL

clean:
	rm -rf $(YMLDIR)
	rm -rf $(OUTDIR)
	rm -rf $(MAILDATA)

%.test: $(OUTDIR)/%.suc

$(OUTDIR):
	mkdir $(OUTDIR)

$(MAILDATA):
	ls -1 $(dir $(MAILDATA))*.txt > $(MAILDATA)
	@chmod u+w $(MAILDATA)

$(YMLDIR):
	mkdir $(YMLDIR)

$(YMLDIR)/%.yml: $(INDIR)/%.yml.in
	$(FIXUP) $(INDIR)/$*.yml.in
	@mv $(INDIR)/$*.yml $(YMLDIR)/.

$(OUTDIR)/%.suc: $(MAPRED) $(YMLDIR)/%.yml $(MASTERDIR)/%.out $(MASTERDIR)/%.err
	@echo [TESTING] $(YMLDIR)/$*.yml
	@rm -f $(OUTDIR)/$*.suc $(OUTDIR)/$*.fail
	@touch $(OUTDIR)/$*.out $(OUTDIR)/$*.err
	@cp -f $(MASTERDIR)/DEFAULT.out $(OUTDIR)/$*.out
	@cp -f $(MASTERDIR)/DEFAULT.err $(OUTDIR)/$*.err
	@$(MAPRED) $(MFLAGS) $(YMLDIR)/$*.yml >>$(OUTDIR)/$*.out 2>>$(OUTDIR)/$*.err
	@$(MRDIFF) $(OUTDIR) $(MASTERDIR) $* > $(OUTDIR)/$*.suc
	@cd $(OUTDIR); if [ -s $*.suc ] ; then mv $*.suc $*.fail; echo $*.fail >> FAIL ; fi	
