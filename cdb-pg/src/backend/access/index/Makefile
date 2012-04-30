#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/index
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/index/Makefile,v 1.14 2008/02/19 10:30:06 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/index
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = genam.o indexam.o catquery.o

include $(top_srcdir)/src/backend/common.mk

# Build a list of known source files with CaQL statements -- should
# rebuild catquery.c if any of these change.
# Note that caql.files "find" will discover CaQL statements in files
# that are *not* in this list, which is correct.  Amend this list as
# needed with latest contents of caql.files to improve Makefile
# dependencies for catquery.c regeneration
CAQL_CQL_SRCS := $(addprefix $(top_srcdir)/src/backend/,\
	catalog/aclchk.c \
	catalog/dependency.c \
	catalog/gp_fastsequence.c \
	catalog/heap.c \
	catalog/index.c \
	catalog/pg_appendonly.c \
	catalog/pg_attribute_encoding.c \
	catalog/pg_compression.c \
	catalog/pg_constraint.c \
	catalog/pg_conversion.c \
	catalog/pg_depend.c \
	catalog/pg_extprotocol.c \
	catalog/pg_exttable.c \
	catalog/pg_largeobject.c \
	catalog/pg_proc.c \
	catalog/pg_proc_callback.c \
	catalog/pg_shdepend.c \
	cdb/cdbcat.c \
	cdb/cdbfts.c \
	cdb/cdbpartition.c \
	cdb/cdbutil.c \
	commands/analyze.c \
	commands/async.c \
	commands/cluster.c \
	commands/comment.c \
	commands/dbcommands.c \
	commands/extprotocolcmds.c \
	commands/filespace.c \
	commands/foreigncmds.c \
	commands/functioncmds.c \
	commands/indexcmds.c \
	commands/opclasscmds.c \
	commands/proclang.c \
	commands/queue.c \
	commands/tablecmds.c \
	commands/tablespace.c \
	commands/trigger.c \
	commands/typecmds.c \
	commands/user.c \
	commands/vacuum.c \
	fts/fts.c \
	optimizer/util/plancat.c \
	parser/analyze.c \
	parser/parse_func.c \
	postmaster/autovacuum.c \
	rewrite/rewriteRemove.c \
	storage/large_object/inv_api.c \
	utils/adt/regproc.c \
	utils/adt/ruleutils.c \
	utils/cache/relcache.c \
	utils/gp/segadmin.c \
	utils/resscheduler/resscheduler.c \
	)

# Generate catquery.c by discovering cql statements in src files

jversion := 5.0
# comment out next line to eliminate run-time logging of CaQL basic functions
#logquery_FLAGS := -logquery 

caql.files: $(CAQL_CQL_SRCS) 
	find $(top_srcdir)/src/ -name '*.c' | \
	xargs grep -l cql | grep -v catquery > caql.files 

catquery.o: catquery.c

catquery.c: caql.files $(top_srcdir)/src/include/catalog/calico.pl
	perl $(top_srcdir)/src/include/catalog/calico.pl \
		-meta $(top_srcdir)/../gpMgmt/bin/gppylib/data/$(jversion).json \
		-syscache $(top_srcdir)/src/backend/utils/cache/syscache.c \
		$(logquery_FLAGS) \
		-filemap caqlfilemap.json \
		-gperf gperf.init -infiles caql.files > catquery.c 
	# track the cql source files
	cat caql.files | xargs openssl dgst -md5 -hex | cat > caql.md5 

clean:
	rm -f SUBSYS.o $(OBJS) catquery.c caql.files gperf.init caqlfilemap.json

