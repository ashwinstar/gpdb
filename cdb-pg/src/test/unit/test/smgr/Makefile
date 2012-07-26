BIN=smgr_test
include ../Makefile.all
include ${CMOCKERY_DIR}/Makefile
include ${MOCK_DIR}/Makefile

TEST_OBJS=smgr_test.o
SUT_OBJS=$(CDB_BACKEND_DIR)/storage/smgr/smgr.o
MOCK_OBJS=$(MOCK_DIR)/aset_mock.o \
	$(MOCK_DIR)/bufmgr_mock.o \
	$(MOCK_DIR)/cdbcat_mock.o \
	$(MOCK_DIR)/cdbfilerepprimary_mock.o \
	$(MOCK_DIR)/cdbmirroredappendonly_mock.o \
	$(MOCK_DIR)/cdbpersistentdatabase_mock.o \
	$(MOCK_DIR)/cdbpersistentfilespace_mock.o \
	$(MOCK_DIR)/cdbpersistentfilesysobj_mock.o \
	$(MOCK_DIR)/cdbpersistentrelation_mock.o \
	$(MOCK_DIR)/cdbpersistenttablespace_mock.o \
	$(MOCK_DIR)/cdbtm_mock.o \
	$(MOCK_DIR)/elog_mock.o \
	$(MOCK_DIR)/faultinjector_mock.o \
	$(MOCK_DIR)/freespace_mock.o \
	$(MOCK_DIR)/guc_mock.o \
	$(MOCK_DIR)/ipc_mock.o \
	$(MOCK_DIR)/lwlock_mock.o \
	$(MOCK_DIR)/mcxt_mock.o \
	$(MOCK_DIR)/md_mock.o \
	$(MOCK_DIR)/persistentendxactrec_mock.o \
	$(MOCK_DIR)/persistentfilesysobjname_mock.o \
	$(MOCK_DIR)/postgres_mock.o \
	$(MOCK_DIR)/s_lock_mock.o \
	$(MOCK_DIR)/shmem_mock.o \
	$(MOCK_DIR)/tablespace_mock.o \
	$(MOCK_DIR)/tuptoaster_mock.o \
	$(MOCK_DIR)/twophase_mock.o \
	$(MOCK_DIR)/xact_mock.o \
	$(MOCK_DIR)/xlog_mock.o \
	$(MOCK_DIR)/xlogutils_mock.o

	
include ../Makefile.all2