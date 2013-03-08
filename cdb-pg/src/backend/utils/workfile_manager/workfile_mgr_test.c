/*-------------------------------------------------------------------------
 *
 * workfile_mgr_test.c
 *	 Unit tests for workfile manager and cache.
 *
 * Copyright (c) 2012, EMC Corp.
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
#include "utils/memutils.h"
#include "utils/builtins.h"
#include "utils/sharedcache.h"
#include "utils/syncrefhashtable.h"
#include "miscadmin.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbgang.h"

#define TEST_NAME_LENGTH 50
#define TEST_HT_NUM_ELEMENTS 8192

typedef struct TestSyncHTElt
{
	char key[TEST_NAME_LENGTH];
	int data;
	int numChildren;
	int32 pinCount;
} TestSyncHTElt;

typedef struct TestCacheElt
{
	char key[TEST_NAME_LENGTH];
	int data;
} TestCacheElt;

int tests_passed;
int tests_failed;
int tests_total;

/* Test definitions */
static bool syncrefhashtable_test_basics(bool enabled);
static bool syncrefhashtable_test_concurrency_non_overlap(bool enabled);
static bool syncrefhashtable_test_error_cases(bool enabled);
static bool syncrefhashtable_test_full_table(bool enabled);

static bool cache_test_acquire(bool enabled);
static bool cache_test_insert(bool enabled);
static bool cache_test_remove(bool enabled);
static bool cache_test_concurrency(bool enabled);
static bool cache_test_evict(bool enabled);
static bool cache_test_evict_stress(bool enabled);
static bool cache_test_evict_threaded(bool enabled);

/* Local helpers */
static SyncHT *syncrefhastable_test_create(void);
static Cache *cache_test_create();

/* Unit tests helper */
static void unit_test_result(bool result);
static void unit_test_reset(void);
static bool unit_test_summary(void);

Datum
gp_workfile_manager_test_harness(PG_FUNCTION_ARGS)
{
	bool result = true;

	/* SyncRefHashtable tests */
	result = syncrefhashtable_test_basics(false /* enabled */) && result;
	result = syncrefhashtable_test_concurrency_non_overlap(false /* enabled */) && result;
	result = syncrefhashtable_test_error_cases(false /* enabled */) && result;
	result = syncrefhashtable_test_full_table(false /* enabled */) && result;

	/* Cache tests */
	result = cache_test_acquire(false /* enabled */) && result;
	result = cache_test_insert(false /* enabled */) && result;
	result = cache_test_remove(false /* enabled */) && result;
	result = cache_test_concurrency(false /* enabled */) && result;
	result = cache_test_evict(false /* enabled */) && result;
	result = cache_test_evict_stress(true /* enabled */) && result;
	result = cache_test_evict_threaded(false /* enabled */) && result;

	PG_RETURN_BOOL(result);
}

/*
 * Callback function to test if two cache resources are equivalent.
 */
static bool
cacheEltEquivalent(const void *resource1, const void *resource2)
{
	TestCacheElt *elt1 = (TestCacheElt *) resource1;
	TestCacheElt *elt2 = (TestCacheElt *) resource2;

	return elt1->data == elt2->data &&
			strncmp(elt1->key, elt2->key, TEST_NAME_LENGTH) == 0;
}

static void
cacheEltCleanup(const void *resource)
{
	TestCacheElt *elt = (TestCacheElt *) resource;

	CACHE_ASSERT_VALID(elt);
	elt->data = -1;
}

/*
 * Function to be called to cleanup in event of client error
 *
 * Releases and surrenders all client owned entries back to the cache
 */
static void
Cache_TeardownCallback(XactEvent event, void *arg)
{
	Assert(NULL != arg);

	Cache *cache = (Cache *) arg;

	elog(LOG, "Calling cleanup now for cache %s with %d used entries",
			cache->cacheName,
			cache->cacheHdr->nEntries - cache->cacheHdr->cacheStats.noFreeEntries);

	Cache_SurrenderClientEntries(cache);
}

static Cache *
cache_test_create()
{
	CacheCtl cacheCtl;
	MemSet(&cacheCtl, 0, sizeof(CacheCtl));

	cacheCtl.entrySize = sizeof(TestCacheElt);
	cacheCtl.keySize = TEST_NAME_LENGTH;
	cacheCtl.keyOffset = GPDB_OFFSET(TestCacheElt, key);

	cacheCtl.hash = string_hash;
	cacheCtl.keyCopy = (HashCopyFunc) strncpy;
	cacheCtl.match = (HashCompareFunc) strncmp;

	cacheCtl.equivalentEntries = cacheEltEquivalent;
	cacheCtl.cleanupEntry = cacheEltCleanup;

	cacheCtl.maxSize = TEST_HT_NUM_ELEMENTS;
	cacheCtl.cacheName = "Test Cache";

	cacheCtl.baseLWLockId = FirstWorkfileMgrLock;
	cacheCtl.numPartitions = NUM_WORKFILEMGR_PARTITIONS;


	Cache *cache = NULL;
	cache = Cache_Create(&cacheCtl);

	Assert(cache);
	RegisterXactCallbackOnce(Cache_TeardownCallback, cache);

	return cache;
}

static bool
cache_test_acquire(bool enabled)
{
	if (!enabled)
	{
		return true;
	}

	unit_test_reset();

	elog(INFO, "Running test: cache_test_acquire");

	elog(INFO, "Running sub-test: CacheCreate");
	Cache *cache = cache_test_create();
	unit_test_result(cache != NULL);

	elog(INFO, "Running sub-test: CacheAcquireEntry");
	CacheEntry *entry = Cache_AcquireEntry(cache);
	unit_test_result(entry != NULL);

	TestCacheElt *elt = CACHE_ENTRY_PAYLOAD(entry);

	strncpy(elt->key, "Test Key 1", TEST_NAME_LENGTH);
	elt->data = 4567;

	elog(INFO, "Running sub-test: CacheSurrenderEntry");
	Cache_SurrenderEntry(cache, entry);
	unit_test_result(true);

	return unit_test_summary();
}

static bool
cache_test_insert(bool enabled)
{
	if (!enabled)
	{
		return true;
	}

	unit_test_reset();

	elog(INFO, "Running test: cache_test_insert");

	elog(INFO, "Running sub-test: CacheCreate");
	Cache *cache = cache_test_create();
	unit_test_result(cache != NULL);

	elog(INFO, "Running sub-test: CacheAcquireEntry");
	CacheEntry *entry = Cache_AcquireEntry(cache);
	unit_test_result(entry != NULL);

	elog(INFO, "Running sub-test: CacheInsert");
	TestCacheElt *elt = CACHE_ENTRY_PAYLOAD(entry);
	strncpy(elt->key, "Test Key 2", TEST_NAME_LENGTH);
	elt->data = 1111;

	Cache_Insert(cache, entry);
	Cache_Release(cache, entry);
	unit_test_result(true);

	/* Look-up test */
	elog(INFO, "Running sub-test: CacheLookup");
	CacheEntry *localEntry = Cache_AcquireEntry(cache);
	elt = CACHE_ENTRY_PAYLOAD(localEntry);
	strncpy(elt->key, "Test Key 2", TEST_NAME_LENGTH);
	elt->data = 1111;

	CacheEntry *lookedUpEntry = Cache_Lookup(cache, localEntry);
	unit_test_result(lookedUpEntry != NULL);

	Cache_Release(cache, lookedUpEntry);

	elog(INFO, "Running sub-test: CacheLookup equal key, no match on value");
	elt = CACHE_ENTRY_PAYLOAD(localEntry);
	strncpy(elt->key, "Test Key 2", TEST_NAME_LENGTH);
	elt->data = 1234;
	lookedUpEntry = Cache_Lookup(cache, localEntry);
	unit_test_result(lookedUpEntry == NULL);

	elog(INFO, "Running sub-test: CacheLookup different key, no match");
	elt = CACHE_ENTRY_PAYLOAD(localEntry);
	strncpy(elt->key, "Test Key bogus", TEST_NAME_LENGTH);
	elt->data = 1111;
	lookedUpEntry = Cache_Lookup(cache, localEntry);
	unit_test_result(lookedUpEntry == NULL);

	Cache_SurrenderEntry(cache, localEntry);
	return unit_test_summary();

}

static bool
cache_test_remove(bool enabled)
{
	if (!enabled)
	{
		return true;
	}

	unit_test_reset();

	elog(INFO, "Running test: cache_test_remove");

	elog(INFO, "Running sub-test: CacheCreate");
	Cache *cache = cache_test_create();
	unit_test_result(cache != NULL);

	char *testKey = "Test Remove Key 2";

	/* Insert one entry */
	CacheEntry *entry1 = Cache_AcquireEntry(cache);
	TestCacheElt *elt1 = CACHE_ENTRY_PAYLOAD(entry1);
	strncpy(elt1->key, testKey, TEST_NAME_LENGTH);
	elt1->data = 1111;
	Cache_Insert(cache, entry1);
	Cache_Release(cache, entry1);

	/* Insert another entry */
	CacheEntry *entry2 = Cache_AcquireEntry(cache);
	TestCacheElt *elt2 = CACHE_ENTRY_PAYLOAD(entry2);
	strncpy(elt2->key, testKey, TEST_NAME_LENGTH);
	elt2->data = 2222;
	Cache_Insert(cache, entry2);
	Cache_Release(cache, entry2);

	/* Look-up and remove an entry */
	CacheEntry *localEntry = Cache_AcquireEntry(cache);
	TestCacheElt *elt3 = CACHE_ENTRY_PAYLOAD(localEntry);
	strncpy(elt3->key, testKey, TEST_NAME_LENGTH);
	elt3->data = 2222;

	elog(INFO, "Running sub-test: Look-up inserted element");
	CacheEntry *lookedUpEntry = Cache_Lookup(cache, localEntry);
	unit_test_result(lookedUpEntry != NULL);

	elog(INFO, "Running sub-test: Remove looked-up element");
	Cache_Remove(cache, lookedUpEntry);
	Cache_Release(cache, lookedUpEntry);
	unit_test_result(true);

	elog(INFO, "Running sub-test: Look up removed element");
	elt3 = CACHE_ENTRY_PAYLOAD(localEntry);
	strncpy(elt3->key, testKey, TEST_NAME_LENGTH);
	elt3->data = 2222;
	lookedUpEntry = Cache_Lookup(cache, localEntry);
	unit_test_result(lookedUpEntry == NULL);

	elog(INFO, "Running sub-test: Look up existing element");
	elt3 = CACHE_ENTRY_PAYLOAD(localEntry);
	strncpy(elt3->key, testKey, TEST_NAME_LENGTH);
	elt3->data = 1111;
	lookedUpEntry = Cache_Lookup(cache, localEntry);
	unit_test_result(lookedUpEntry != NULL);

	Cache_SurrenderEntry(cache, localEntry);
	Cache_Release(cache, lookedUpEntry);

	return unit_test_summary();
}

static bool
cache_test_concurrency(bool enabled)
{
	if (!enabled)
	{
		return true;
	}

	unit_test_reset();

	elog(INFO, "Running test: cache_test_concurrency");

	elog(INFO, "Running sub-test: CacheCreate");
	Cache *cache = cache_test_create();
	unit_test_result(cache != NULL);

	/* Number of elements in the array to hold test entries */
	const int noTestEntries = 1000;
	CacheEntry *entries[noTestEntries];
	char key[TEST_NAME_LENGTH];

	elog(INFO, "Running sub-test: Cache insert/lookup/delete many elements");
	int noTestIterations = 5000;
	int i = 0;
	int iterNo = 0;
	bool testFailed = false;

	for (iterNo = 0; iterNo < noTestIterations ; iterNo ++)
	{
		if (testFailed)
		{
			break;
		}

		/* Insert noTestEntries elements */
		for (i=0;i<noTestEntries;i++)
		{
			/* If we include Pid in the key, we get short chains */
			/* snprintf(key, TEST_NAME_LENGTH, "PID=%d cache key no. %d", MyProcPid, i); */
			snprintf(key, TEST_NAME_LENGTH, "cache key no. %d", i);

			entries[i] = Cache_AcquireEntry(cache);

			if (entries[i] == NULL)
			{
				elog(INFO, "Could not acquire entry");
				testFailed = true;
				break;
			}
			TestCacheElt *elt = CACHE_ENTRY_PAYLOAD(entries[i]);
			strncpy(elt->key, key, TEST_NAME_LENGTH);
			elt->data = MyProcPid;

			Cache_Insert(cache, entries[i]);
			Cache_Release(cache, entries[i]);
		}

		if (testFailed)
		{
			break;
		}

		/* Look up noTestEntries */
		for (i=0; i<noTestEntries; i++)
		{
			CacheEntry *localEntry = Cache_AcquireEntry(cache);
			if (localEntry == NULL)
			{
				elog(INFO, "Could not acquire entry");
				testFailed = true;
				break;
			}
			TestCacheElt *elt = CACHE_ENTRY_PAYLOAD(localEntry);
			//snprintf(key, TEST_NAME_LENGTH, "PID=%d cache key no. %d", MyProcPid, i);
			snprintf(key, TEST_NAME_LENGTH, "cache key no. %d", i);
			strncpy(elt->key, key, TEST_NAME_LENGTH);
			elt->data = MyProcPid;

			entries[i] = Cache_Lookup(cache, localEntry);
			if (entries[i] == NULL)
			{
				elog(INFO, "Could not find inserted entry");
				testFailed = true;
				break;
			}

			Cache_SurrenderEntry(cache, localEntry);
		}

		if (testFailed)
		{
			break;
		}

		/* Delete and release noTestEntries */
		for (i=0; i<noTestEntries; i++)
		{
			Cache_Remove(cache, entries[i]);
			Cache_Release(cache, entries[i]);
		}

		CHECK_FOR_INTERRUPTS();

		/* Look up no TestEntries again, should not be found */
		for (i=0; i<noTestEntries; i++)
		{
			CacheEntry *localEntry = Cache_AcquireEntry(cache);
			if (localEntry == NULL)
			{
				elog(INFO, "Could not acquire entry");
				testFailed = true;
				break;
			}
			TestCacheElt *elt = CACHE_ENTRY_PAYLOAD(localEntry);
			//snprintf(key, TEST_NAME_LENGTH, "PID=%d cache key no. %d", MyProcPid, i);
			snprintf(key, TEST_NAME_LENGTH, "cache key no. %d", i);
			strncpy(elt->key, key, TEST_NAME_LENGTH);
			elt->data = MyProcPid;

			entries[i] = Cache_Lookup(cache, localEntry);
			if (entries[i] != NULL)
			{
				elog(INFO, "Unexpected entry found in cache");
				testFailed = true;
				break;
			}

			Cache_SurrenderEntry(cache, localEntry);
		}

		CHECK_FOR_INTERRUPTS();
	}

	unit_test_result(!testFailed);
	return unit_test_summary();
}

static bool
cache_test_evict(bool enabled)
{
	if (!enabled)
	{
		return true;
	}

	unit_test_reset();

	elog(INFO, "Running test: cache_test_evict");

	elog(INFO, "Running sub-test: CacheCreate");
	Cache *cache = cache_test_create();
	unit_test_result(cache != NULL);

	/* Number of elements in the array to hold test entries */
	const int noTestEntries = 20;
	const int entryWeight = 3;
	CacheEntry *entries[noTestEntries];
	char key[TEST_NAME_LENGTH];


	/* Inserting noTestEntries entries */
	elog(INFO, "Running sub-test: Inserting elements");
	int i;
	for (i=0;i<noTestEntries;i++)
	{
		snprintf(key, TEST_NAME_LENGTH, "PID=%d cache key no. %d", MyProcPid, i);
		entries[i] = Cache_AcquireEntry(cache);
		Assert(NULL != entries[i]);

		TestCacheElt *elt = CACHE_ENTRY_PAYLOAD(entries[i]);
		strncpy(elt->key, key, TEST_NAME_LENGTH);
		elt->data = MyProcPid;

		entries[i]->size = entryWeight;
		entries[i]->utility = random() % 100;

		Cache_Insert(cache, entries[i]);
		Cache_Release(cache, entries[i]);
	}
	unit_test_result(true);

	/* Evicting elements */
	elog(INFO, "Running sub-test: Succesful eviction");

	uint32 evictRequest = (noTestEntries * entryWeight) / 2 + 1; 	/* Half of the entries + 1 */
	uint32 evictActual = Cache_Evict(cache, evictRequest);

	/* Expected result: Evicted half plus one entries */
	unit_test_result(evictActual == evictRequest - 1 + entryWeight);

	elog(INFO, "Running sub-test: Unsuccesful eviction");
	evictRequest = (noTestEntries * entryWeight) / 2; /* Half of the entries */
	evictActual = Cache_Evict(cache, evictRequest);

	/* Expected result: Evicted one less entry than requested */
	unit_test_result(evictActual == evictRequest - entryWeight);
	return unit_test_summary();
}

static bool
cache_test_evict_stress(bool enabled)
{
	if (!enabled)
	{
		return true;
	}

	unit_test_reset();

	elog(INFO, "Running test: cache_test_evict_stress");

	elog(INFO, "Running sub-test: CacheCreate");
	Cache *cache = cache_test_create();
	unit_test_result(cache != NULL);

	/* Number of elements in the array to hold test entries */
	const int noTestEntries = 1000;
	const int entryWeight = 3;
	CacheEntry *entries[noTestEntries];
	char key[TEST_NAME_LENGTH];
	bool testFailed = false;
	const uint32 noTestIterations = 1000;
	uint32 iterNo;

	elog(INFO, "Running sub-test: Cache insert/lookup/evict many elements");
	for (iterNo = 0; iterNo < noTestIterations ; iterNo ++)
	{
		if (testFailed)
		{
			break;
		}

		/* Inserting noTestEntries entries */
		int i;
		for (i=0;i<noTestEntries;i++)
		{
			/* If we include Pid in the key, we get short chains */
			/* snprintf(key, TEST_NAME_LENGTH, "PID=%d cache key no. %d", MyProcPid, i); */
			snprintf(key, TEST_NAME_LENGTH, "cache key no. %d", i);
			entries[i] = Cache_AcquireEntry(cache);
			Assert(NULL != entries[i]);

			TestCacheElt *elt = CACHE_ENTRY_PAYLOAD(entries[i]);
			strncpy(elt->key, key, TEST_NAME_LENGTH);
			elt->data = MyProcPid;

			entries[i]->size = entryWeight;
			entries[i]->utility = random() % 100;

			Cache_Insert(cache, entries[i]);
			Cache_Release(cache, entries[i]);
		}

		if (testFailed)
		{
			break;
		}

		/* Look up noTestEntries */
		for (i=0; i<noTestEntries; i++)
		{
			CacheEntry *localEntry = Cache_AcquireEntry(cache);
			if (localEntry == NULL)
			{
				elog(INFO, "Could not acquire entry");
				testFailed = true;
				break;
			}
			TestCacheElt *elt = CACHE_ENTRY_PAYLOAD(localEntry);
			/* snprintf(key, TEST_NAME_LENGTH, "PID=%d cache key no. %d", MyProcPid, i); */
			snprintf(key, TEST_NAME_LENGTH, "cache key no. %d", i);
			strncpy(elt->key, key, TEST_NAME_LENGTH);
			elt->data = MyProcPid;

			entries[i] = Cache_Lookup(cache, localEntry);
			/*
			 * Since we're possibly running evictions from other clients in the meantime,
			 * some of these elements will not be found. But that's ok, we can still
			 * look them up to exercise that mechanism
			 */

			if (NULL != entries[i])
			{
				Cache_Release(cache, entries[i]);
			}

			Cache_SurrenderEntry(cache, localEntry);
		}

		if (testFailed)
		{
			break;
		}

		/* Evict noTestEntries x weight from cache */
		uint32 evictRequest = noTestEntries * entryWeight;
		uint32 evictSize = Cache_Evict(cache, evictRequest);

		/* XXX Under high concurrency, this test can actually legally fail.
		 * If someone else just evicted everything we added, and we have to
		 * wait for someone else to insert something we can evict, but it's
		 * happening too slowly
		 */

		if (evictSize != evictRequest)
		{
			elog(INFO, "Could not satisfy evict. Requested=%u, evicted=%u", evictRequest, evictSize);
			testFailed = true;
			break;
		}

		CHECK_FOR_INTERRUPTS();
	}

	unit_test_result(!testFailed);
	return unit_test_summary();
}

typedef struct Cache_TestThreadParam
{
	/*
	 * The pthread_t thread handle.
	 */
	pthread_t	thread;
	bool		thread_valid;
	uint32 threadno;
} Cache_TestThreadParam;


/*
 * thread_DispatchCommand is the thread proc used to dispatch the command to one or more of the qExecs.
 *
 * NOTE: This function MUST NOT contain elog or ereport statements. (or most any other backend code)
 *		 elog is NOT thread-safe.  Developers should instead use something like:
 *
 *	if (DEBUG3 >= log_min_messages)
 *			write_log("my brilliant log statement here.");
 *
 * NOTE: In threads, we cannot use palloc, because it's not thread safe.
 */
static void *
thread_test_evict(void *arg)
{

	gp_set_thread_sigmasks();

	Cache_TestThreadParam *pParam = (Cache_TestThreadParam *) arg;
	write_log("my brilliant log statement has arrived from thread %d", pParam->threadno);

	return NULL;
}

static bool
cache_test_evict_threaded(bool enabled)
{
	if (!enabled)
	{
		return true;
	}

	unit_test_reset();

	elog(INFO, "Running test: cache_test_evict_threaded");

	elog(INFO, "Running sub-test: CacheCreate");
	Cache *cache = cache_test_create();
	unit_test_result(cache != NULL);

	int noThreads = 5;

	Cache_TestThreadParam *pParmsArr =
			(Cache_TestThreadParam *) palloc(noThreads * sizeof(Cache_TestThreadParam));

	int i = 0;
	int pthread_err = 0;

	/* Start threads */
	for (i=0;i<noThreads; i++)
	{
		Cache_TestThreadParam *pParms = &pParmsArr[i];
		pParms->threadno = i;
		pthread_err = gp_pthread_create(&pParms->thread, thread_test_evict, pParms, "thread_test_evict");
		Assert(0 == pthread_err);

	}

	/* Wait for threads to finish */
	for (i=0;i<noThreads; i++)
	{
		Cache_TestThreadParam *pParms = &pParmsArr[i];
		if (pParms->thread_valid)
		{
			int			pthread_err = 0;
			pthread_err = pthread_join(pParms->thread, NULL);
			Assert(0 == pthread_err);
		}

	}

	return unit_test_summary();
}

/*
 * Callback function to test if an entry in the hashtable is "empty"
 */
static bool
isTestEltEmpty(const void *entry)
{
	TestSyncHTElt *testElt = (TestSyncHTElt *) entry;
	return testElt->numChildren == 0;
}

/*
 * Callback function to initialize an entry before returning to the caller
 */
static void
initTestElt(void *entry)
{
	TestSyncHTElt *testElt = (TestSyncHTElt *) entry;
	testElt->numChildren = 0;
	testElt->pinCount = 0;
}

static bool
syncrefhashtable_test_basics(bool enabled)
{
	if (!enabled)
	{
		return true;
	}

	unit_test_reset();

	elog(INFO, "Running test: syncrefhashtable_test_basics");

	elog(INFO, "Running sub-test: SyncHTCreate");
	SyncHT *syncHT = syncrefhastable_test_create();
	unit_test_result(syncHT != NULL);
	Assert(syncHT);

	char *keyText = "Key one";

	elog(INFO, "Running sub-test: SyncHTInsert first element");
	bool existing = false;

	/* Number of elements in the array to hold test entries */
	const int noTestEntries = 20;

	TestSyncHTElt *elements[noTestEntries];
	long numEntries = 0;

	elements[0] = (TestSyncHTElt *) SyncHTInsert(syncHT, (void *) keyText, &existing);
	unit_test_result(elements[0] != NULL && !existing);

	elog(INFO, "Running sub-test: SyncHTInsert duplicate element");
	existing = false;
	elements[1] = (TestSyncHTElt *) SyncHTInsert(syncHT, (void *) keyText, &existing);
	unit_test_result(elements[1] != NULL && existing);

	elog(INFO, "Running sub-test: SyncHTInsert unique element");
	keyText = "Key two";
	existing = false;
	elements[2] = (TestSyncHTElt *) SyncHTInsert(syncHT, (void *) keyText, &existing);
	unit_test_result(elements[2] != NULL && !existing);

	elog(INFO, "Running sub-test: SyncHTRelease");
	bool deleted[noTestEntries];
	deleted[0] = SyncHTRelease(syncHT, elements[0]);
	deleted[1] = SyncHTRelease(syncHT, elements[1]);
	deleted[2] = SyncHTRelease(syncHT, elements[2]);
	numEntries = SyncHTNumEntries(syncHT);

	unit_test_result(numEntries == 0 && !deleted[0] && deleted[1] && deleted[2]);


	elog(INFO, "Running sub-test: SyncHTDestroy");
	SyncHTDestroy(syncHT);
	unit_test_result(true);

	return unit_test_summary();
}

static bool
syncrefhashtable_test_concurrency_non_overlap(bool enabled)
{
	if (!enabled)
	{
		return true;
	}

	unit_test_reset();

	elog(INFO, "Running test: syncrefhashtable_test_concurrency_non_overlap");

	elog(INFO, "Running sub-test: SyncHTCreate");
	SyncHT *syncHT = syncrefhastable_test_create();
	unit_test_result(syncHT != NULL);
	Assert(syncHT);

	/* Number of elements in the array to hold test entries */
	const int noTestEntries = 1000;
	TestSyncHTElt *elements[noTestEntries];
	bool existing = false, deleted = false;

	char key[TEST_NAME_LENGTH];

	elog(INFO, "Running sub-test: SyncHTInsert many elements");
	int noTestIterations = 5000;
	int i = 0;
	int iterNo = 0;
	bool testFailed = false;

	for (iterNo = 0; iterNo < noTestIterations ; iterNo ++)
	{
		if (testFailed)
		{
			break;
		}

		/* Insert noTestEntries elements */
		for (i=0;i<noTestEntries;i++)
		{
			snprintf(key, TEST_NAME_LENGTH, "PID=%d key no. %d", MyProcPid, i);

			elements[i] = SyncHTInsert(syncHT, key, &existing);
			if (existing)
			{
				testFailed = true;
				break;
			}
			elements[i]->numChildren = 123;
		}

		/* Release noTestEntries elements, don't get deleted */
		for (i=0; i < noTestEntries; i++)
		{
			deleted = SyncHTRelease(syncHT, elements[i]);
			if (deleted)
			{
				testFailed = true;
				break;
			}
		}

//		numEntries = SyncHTNumEntries(syncHT); /* XXX This does not hold for concurrency */
//		if (numEntries != noTestEntries)
//		{
//			testFailed = true;
//			break;
//		}


		/* Look-up noTestEntries elements. Empty them and release them, they should get deleted */
		for (i=0;i<noTestEntries;i++)
		{
			snprintf(key, TEST_NAME_LENGTH, "PID=%d key no. %d", MyProcPid, i);
			TestSyncHTElt *foundElt = (TestSyncHTElt *) SyncHTLookup(syncHT, key);
			if (!foundElt)
			{
				testFailed = true;
				break;
			}
			/* Logically empty the element */
			foundElt->numChildren = 0;

			/* Release element. It should get deleted. */
			deleted = SyncHTRelease(syncHT, foundElt);
			if (!deleted)
			{
				testFailed = true;
				break;
			}
		}

//		numEntries = SyncHTNumEntries(syncHT); /* XXX This does not hold for concurrency */
//		if (numEntries != 0)
//		{
//			testFailed = true;
//			break;
//		}
	} /* for iterNo */

	unit_test_result(!testFailed);

	elog(INFO, "Running sub-test: SyncHTDestroy");
	SyncHTDestroy(syncHT);
	unit_test_result(true);

	return unit_test_summary();
}

static bool
syncrefhashtable_test_error_cases(bool enabled)
{
	if (!enabled)
	{
		return true;
	}

	unit_test_reset();

	elog(INFO, "Running test: syncrefhashtable_test_error_cases");

	elog(INFO, "Running sub-test: SyncHTCreate");
	SyncHT *syncHT = syncrefhastable_test_create();
	unit_test_result(syncHT != NULL);
	Assert(syncHT);

	elog(INFO, "Running sub-test: SyncHTInsert double remove");

	TestSyncHTElt *element;
	char *keyText = "Key one";
	bool existing;
	bool deleted;

	element = SyncHTInsert(syncHT, keyText, &existing);
	deleted = SyncHTRelease(syncHT, element);
	Assert(deleted);

	PG_TRY();
	{
		deleted = SyncHTRelease(syncHT, element);

		/* Should not get here, above line throws */
		unit_test_result(false);
	}
	PG_CATCH();
	{
		unit_test_result(true);
	}
	PG_END_TRY();

	return unit_test_summary();
}

static bool
syncrefhashtable_test_full_table(bool enabled)
{
	if (!enabled)
	{
		return true;
	}

	unit_test_reset();

	elog(INFO, "Running test: syncrefhashtable_test_error_cases");

	elog(INFO, "Running sub-test: SyncHTCreate");
	SyncHT *syncHT = syncrefhastable_test_create();
	unit_test_result(syncHT != NULL);
	Assert(syncHT);

	elog(INFO, "Running sub-test: SyncHTInsert into full hashtable");

	/* Number of elements in the array to hold test entries */
	TestSyncHTElt *elements[10 * TEST_HT_NUM_ELEMENTS + 1];
	char key[TEST_NAME_LENGTH];
	bool existing = false, deleted = false;
	bool testFailed = false;
	int i;

	/* Fill up the hashtable */
	for (i=0;i<2 * TEST_HT_NUM_ELEMENTS;i++)
	{
		snprintf(key, TEST_NAME_LENGTH, "PID=%d key no. %d", MyProcPid, i);

		elements[i] = SyncHTInsert(syncHT, key, &existing);
		if (existing || !elements[i])
		{
			elog(INFO, "Filling up the hashtable failed after %d inserts", i + 1);
			testFailed = true;
			break;
		}
	}

	int numEntries = SyncHTNumEntries(syncHT); /* XXX This does not hold for concurrency */
	elog(INFO, "Hashtable says it hash %d entries", numEntries);

	unit_test_result(!testFailed && numEntries == TEST_HT_NUM_ELEMENTS);

	/* Insert elements in full hashtable */
	snprintf(key, TEST_NAME_LENGTH, "PID=%d key no. %d", MyProcPid, TEST_HT_NUM_ELEMENTS);
	elements[TEST_HT_NUM_ELEMENTS] = SyncHTInsert(syncHT, key, &existing);

	/* Insertion should has failed */
	unit_test_result(elements[TEST_HT_NUM_ELEMENTS] == NULL && !existing);


	/* Look-up noTestEntries elements. Release them, they should get deleted */
	for (i=0;i<TEST_HT_NUM_ELEMENTS;i++)
	{
		snprintf(key, TEST_NAME_LENGTH, "PID=%d key no. %d", MyProcPid, i);
		TestSyncHTElt *foundElt = (TestSyncHTElt *) SyncHTLookup(syncHT, key);
		if (!foundElt)
		{
			testFailed = true;
			break;
		}

		/* Release element. It should get deleted. */
		deleted = SyncHTRelease(syncHT, foundElt);
		if (!deleted)
		{
			testFailed = true;
			break;
		}
	}

	unit_test_result(!testFailed);

	return unit_test_summary();
}

static bool
unit_test_summary(void)
{
	elog(INFO, "Unit tests summary: PASSED: %d/%d, FAILED: %d/%d",
			tests_passed, tests_total,
			tests_failed, tests_total);
	return tests_failed == 0;
}

static void
unit_test_reset()
{
	tests_passed = tests_failed = tests_total = 0;
}

static void
unit_test_result(bool result)
{
	tests_total++;

	if (result)
	{
		tests_passed++;
		elog(INFO, "====== PASS ======");
	}
	else
	{
		tests_failed++;
		elog(INFO, "!!!!!! FAIL !!!!!!");
	}
}

static SyncHT *
syncrefhastable_test_create(void)
{
	SyncHTCtl syncHTCtl;
	MemSet(&syncHTCtl, 0, sizeof(SyncHTCtl));

	syncHTCtl.entrySize = sizeof(TestSyncHTElt);
	syncHTCtl.keySize = TEST_NAME_LENGTH;

	syncHTCtl.hash = string_hash;
	syncHTCtl.keyCopy = (HashCopyFunc) strncpy;
	syncHTCtl.match = (HashCompareFunc) strncmp;

	syncHTCtl.numElements = TEST_HT_NUM_ELEMENTS;

	syncHTCtl.baseLWLockId = FirstWorkfileMgrLock;
	syncHTCtl.numPartitions = NUM_WORKFILEMGR_PARTITIONS;

	syncHTCtl.tabName = "Test SyncRef Hashtable";
	syncHTCtl.isEmptyEntry = isTestEltEmpty;
	syncHTCtl.initEntry = initTestElt;

	syncHTCtl.keyOffset = GPDB_OFFSET(TestSyncHTElt, key);
	syncHTCtl.pinCountOffset = GPDB_OFFSET(TestSyncHTElt, pinCount);

	/* Create hashtable in the top memory context so we can clean up if transaction aborts */
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	SyncHT *syncHT = SyncHTCreate(&syncHTCtl);
	Assert(syncHT);

	MemoryContextSwitchTo(oldcxt);

	return syncHT;
}
