
MAPRED    = ../gpmapreduce
PSQL      = psql
FIXUP     = bin/fixup.pl
MRDIFF    = bin/mrdiff.pl

DB        = mapreduce_test
MFLAGS    = $(DB) --key="clar" --file 
PFLAGS    = $(DB) --file

INDIR     = input
DATADIR   = data
MASTERDIR = expected

OUTDIR    = output
YMLDIR    = yml
SQLDIR    = sql

YMLIN     = $(shell ls $(INDIR)/*.yml.in)
YML       = $(notdir $(basename $(YMLIN)))
YMLOUT    = $(addprefix $(YMLDIR)/, $(YML))

TESTS     = $(addprefix $(OUTDIR)/,$(addsuffix .suc, $(basename $(YML))))
MAILDATA  = $(addprefix $(shell pwd)/,$(DATADIR)/email/mailfiles)

all: setup $(YMLDIR) $(OUTDIR) $(YMLOUT) $(MAILDATA) $(TESTS)
	@if [ -s $(OUTDIR)/FAIL ] ; then                          \
       export nfailed=`cat $(OUTDIR)/FAIL | wc -l`;           \
       export ntotal=`ls -1 $(OUTDIR)/*.suc $(OUTDIR)/*.fail | wc -l`;  \
	   echo [FAIL] $$nfailed /$$ntotal tests failed;           \
	   rm -rf $(OUTDIR)/FAIL;                                 \
	 else                                                     \
       echo [SUCCESS] ;                                       \
     fi

setup:
	@dropdb $(DB) >/dev/null 2>&1 || echo > /dev/null
	@createdb $(DB)
	@createlang -d $(DB) plperl 
	@createlang -d $(DB) plpythonu

clean:
	rm -rf $(YMLDIR)
	rm -rf $(OUTDIR)
	rm -rf $(MAILDATA)
	@dropdb $(DB) 2>/dev/null || echo > /dev/null


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
	@echo $(MAPRED) $(MFLAGS) $(YMLDIR)/$*.yml
	@rm -f $(OUTDIR)/$*.suc $(OUTDIR)/$*.fail
	@touch $(OUTDIR)/$*.out $(OUTDIR)/$*.err
	@cp -f $(MASTERDIR)/DEFAULT.out $(OUTDIR)/$*.out
	@cp -f $(MASTERDIR)/DEFAULT.err $(OUTDIR)/$*.err
	@if [ -e $(SQLDIR)/$*_init.sql ] ; \
	 then $(PSQL) $(PFLAGS) $(SQLDIR)/$*_init.sql > $(OUTDIR)/$*_init.out; fi
	@$(MAPRED) $(MFLAGS) $(YMLDIR)/$*.yml >>$(OUTDIR)/$*.out 2>>$(OUTDIR)/$*.err
	@if [ -e $(SQLDIR)/$*_done.sql ] ; \
	 then $(PSQL) $(PFLAGS) $(SQLDIR)/$*_done.sql > $(OUTDIR)/$*_done.out; fi
	@$(MRDIFF) $(OUTDIR) $(MASTERDIR) $* > $(OUTDIR)/$*.suc
	@cd $(OUTDIR); if [ -s $*.suc ] ; then \
	    mv $*.suc $*.fail; echo $*.fail; echo $*.fail >> FAIL ; fi
