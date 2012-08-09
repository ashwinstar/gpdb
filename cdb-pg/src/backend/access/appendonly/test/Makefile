subdir=src/backend/access/appendonly
top_builddir=../../../../..

TARGETS=aomd

# Objects to be mocked.
aomd_MOCK_OBJS=$(MOCK_DIR)/backend/access/heap/tuptoaster_mock.o \
	$(MOCK_DIR)/backend/access/transam/xact_mock.o \
	$(MOCK_DIR)/backend/catalog/catalog_mock.o \
	$(MOCK_DIR)/backend/cdb/cdbcat_mock.o \
	$(MOCK_DIR)/backend/cdb/cdbmirroredappendonly_mock.o \
	$(MOCK_DIR)/backend/storage/ipc/shmem_mock.o \
	$(MOCK_DIR)/backend/storage/lmgr/s_lock_mock.o \
	$(MOCK_DIR)/backend/tcop/postgres_mock.o \
	$(MOCK_DIR)/backend/utils/cache/relcache_mock.o \
	$(MOCK_DIR)/backend/utils/error/elog_mock.o \
	$(MOCK_DIR)/backend/utils/error/assert_mock.o \
	$(MOCK_DIR)/backend/utils/mmgr/aset_mock.o \
	$(MOCK_DIR)/backend/utils/mmgr/mcxt_mock.o

# Objects from backend, which don't need to be mocked but need to be linked.
aomd_REAL_OBJS=\
	$(top_srcdir)/src/backend/access/hash/hashfunc.o \
	$(top_srcdir)/src/backend/access/transam/filerepdefs.o \
	$(top_srcdir)/src/backend/bootstrap/bootparse.o \
	$(top_srcdir)/src/backend/nodes/bitmapset.o \
	$(top_srcdir)/src/backend/lib/stringinfo.o \
	$(top_srcdir)/src/backend/nodes/equalfuncs.o \
	$(top_srcdir)/src/backend/nodes/list.o \
	$(top_srcdir)/src/backend/parser/gram.o \
	$(top_srcdir)/src/backend/regex/regcomp.o \
	$(top_srcdir)/src/backend/regex/regerror.o \
	$(top_srcdir)/src/backend/regex/regexec.o \
	$(top_srcdir)/src/backend/regex/regfree.o \
	$(top_srcdir)/src/backend/storage/page/itemptr.o \
	$(top_srcdir)/src/backend/utils/adt/datum.o \
	$(top_srcdir)/src/backend/utils/adt/like.o \
	$(top_srcdir)/src/backend/utils/hash/dynahash.o \
	$(top_srcdir)/src/backend/utils/hash/hashfn.o \
	$(top_srcdir)/src/backend/utils/misc/guc.o \
	$(top_srcdir)/src/backend/utils/init/globals.o \
	$(top_srcdir)/src/port/strlcpy.o \
	$(top_srcdir)/src/port/path.o \
	$(top_srcdir)/src/port/pgstrcasecmp.o \
	$(top_srcdir)/src/port/qsort.o \
	$(top_srcdir)/src/port/thread.o \
	$(top_srcdir)/src/timezone/localtime.o \
	$(top_srcdir)/src/timezone/pgtz.o

include ../../../../Makefile.mock
