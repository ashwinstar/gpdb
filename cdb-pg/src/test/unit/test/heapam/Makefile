BIN=heapam_test
include ../Makefile.all
include ${CMOCKERY_DIR}/Makefile

TEST_OBJS=heapam_test.o
SUT_OBJS=$(CDB_BACKEND_DIR)/access/heap/heapam.o $(CDB_BACKEND_DIR)/access/common/heaptuple.o
MOCK_OBJS=$(MOCK_DIR)/aset_mock.o \
	$(MOCK_DIR)/backoff_mock.o \
	$(MOCK_DIR)/bufmgr_mock.o \
	$(MOCK_DIR)/bufpage_mock.o \
	$(MOCK_DIR)/catalog_mock.o \
	$(MOCK_DIR)/cdbcat_mock.o \
	$(MOCK_DIR)/cdbfilerepprimary_mock.o \
	$(MOCK_DIR)/cdbpersistentrecovery_mock.o \
	$(MOCK_DIR)/cdbvars_mock.o \
	$(MOCK_DIR)/combocid_mock.o \
	$(MOCK_DIR)/elog_mock.o \
	$(MOCK_DIR)/faultinjector_mock.o \
	$(MOCK_DIR)/fmgr_mock.o \
	$(MOCK_DIR)/guc_mock.o \
	$(MOCK_DIR)/hio_mock.o \
	$(MOCK_DIR)/inval_mock.o \
	$(MOCK_DIR)/lmgr_mock.o \
	$(MOCK_DIR)/lsyscache_mock.o \
	$(MOCK_DIR)/lwlock_mock.o \
	$(MOCK_DIR)/mcxt_mock.o \
	$(MOCK_DIR)/multixact_mock.o \
	$(MOCK_DIR)/namespace_mock.o \
	$(MOCK_DIR)/pgstat_mock.o \
	$(MOCK_DIR)/postgres_mock.o \
	$(MOCK_DIR)/procarray_mock.o \
	$(MOCK_DIR)/relcache_mock.o \
	$(MOCK_DIR)/s_lock_mock.o \
	$(MOCK_DIR)/shmem_mock.o \
	$(MOCK_DIR)/simex_mock.o \
	$(MOCK_DIR)/syscache_mock.o \
	$(MOCK_DIR)/testutils_mock.o \
	$(MOCK_DIR)/tqual_mock.o \
	$(MOCK_DIR)/transam_mock.o \
	$(MOCK_DIR)/tuptoaster_mock.o \
	$(MOCK_DIR)/xact_mock.o \
	$(MOCK_DIR)/xlog_mock.o \
	$(MOCK_DIR)/xlogutils_mock.o


	
include ../Makefile.all2