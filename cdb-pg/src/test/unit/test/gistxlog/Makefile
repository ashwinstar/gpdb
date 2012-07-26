BIN=gistxlog_test
include ../Makefile.all
include ${CMOCKERY_DIR}/Makefile
include ${MOCK_DIR}/Makefile

TEST_OBJS=gistxlog_test.o
SUT_OBJS=$(CDB_BACKEND_DIR)/access/gist/gistxlog.o
MOCK_OBJS=$(MOCK_DIR)/aset_mock.o \
	$(MOCK_DIR)/bufmgr_mock.o \
	$(MOCK_DIR)/bufpage_mock.o \
	$(MOCK_DIR)/cdbcat_mock.o \
	$(MOCK_DIR)/cdbfilerepprimary_mock.o \
	$(MOCK_DIR)/elog_mock.o \
	$(MOCK_DIR)/gist_mock.o \
	$(MOCK_DIR)/guc_mock.o \
	$(MOCK_DIR)/lwlock_mock.o \
	$(MOCK_DIR)/mcxt_mock.o \
	$(MOCK_DIR)/postgres_mock.o \
	$(MOCK_DIR)/relcache_mock.o \
	$(MOCK_DIR)/s_lock_mock.o \
	$(MOCK_DIR)/shmem_mock.o \
	$(MOCK_DIR)/tuptoaster_mock.o \
	$(MOCK_DIR)/xact_mock.o \
	$(MOCK_DIR)/xlog_mock.o \
	$(MOCK_DIR)/xlogutils_mock.o

		
include ../Makefile.all2