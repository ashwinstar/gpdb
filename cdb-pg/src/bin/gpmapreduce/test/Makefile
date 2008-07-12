
MAPRED    = ../mapred
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

all: $(YMLDIR) $(OUTDIR) $(YMLOUT) $(TESTS) 
	@cd $(OUTDIR); if [ -s FAIL ] ; then cat FAIL ; else echo [SUCCESS] ; fi
	@rm -rf $(OUTDIR)/FAIL

clean:
	rm -rf $(YMLDIR)
	rm -rf $(OUTDIR)

%.test: $(OUTDIR)/%.suc

$(OUTDIR):
	mkdir $(OUTDIR)

$(YMLDIR):
	mkdir $(YMLDIR)

$(YMLDIR)/%.yml: $(INDIR)/%.yml.in
	$(FIXUP) $(INDIR)/$*.yml.in
	@mv $(INDIR)/$*.yml $(YMLDIR)/.

$(OUTDIR)/%.suc: $(MAPRED) $(YMLDIR)/%.yml
	@echo [TESTING] $(YMLDIR)/$*.yml
	@rm -f $(OUTDIR)/$*.suc $(OUTDIR)/$*.fail
	@cp -f $(MASTERDIR)/DEFAULT.out $(OUTDIR)/$*.out
	@cp -f $(MASTERDIR)/DEFAULT.err $(OUTDIR)/$*.err
	@cp -f $(MASTERDIR)/DEFAULT.print.out $(OUTDIR)/$*.print.out
	@cp -f $(MASTERDIR)/DEFAULT.print.err $(OUTDIR)/$*.print.err
	@$(MAPRED) $(MFLAGS) $(YMLDIR)/$*.yml >>$(OUTDIR)/$*.out 2>>$(OUTDIR)/$*.err
	@$(MAPRED) -p $(MFLAGS) $(YMLDIR)/$*.yml >>$(OUTDIR)/$*.print.out 2>>$(OUTDIR)/$*.print.err
	@$(MRDIFF) $(OUTDIR) $(MASTERDIR) $* > $(OUTDIR)/$*.suc
	@cd $(OUTDIR); if [ -s $*.suc ] ; then mv $*.suc $*.fail; echo $*.fail >> FAIL ; fi	
