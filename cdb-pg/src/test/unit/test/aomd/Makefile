BIN=aomd_test
include ../Makefile.all
include ${CMOCKERY_DIR}/Makefile
include ${MOCK_DIR}/Makefile

TEST_OBJS=aomd_test.o
SUT_OBJS=$(CDB_BACKEND_DIR)/access/appendonly/aomd.o
MOCK_OBJS=$(MOCK_DIR)/aset_mock.o \
	$(MOCK_DIR)/catalog_mock.o \
	$(MOCK_DIR)/cdbcat_mock.o \
	$(MOCK_DIR)/cdbmirroredappendonly_mock.o \
	$(MOCK_DIR)/elog_mock.o \
	$(MOCK_DIR)/mcxt_mock.o \
	$(MOCK_DIR)/postgres_mock.o \
	$(MOCK_DIR)/relcache_mock.o \
	$(MOCK_DIR)/s_lock_mock.o \
	$(MOCK_DIR)/shmem_mock.o \
	$(MOCK_DIR)/tuptoaster_mock.o \
	$(MOCK_DIR)/xact_mock.o

	
include ../Makefile.all2