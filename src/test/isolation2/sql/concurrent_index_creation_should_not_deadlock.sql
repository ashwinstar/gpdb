-- Test to make sure without existing index, concurrent index
-- creations on AO won't deadlock

CREATE TABLE concurrent_index_test (value int) WITH (appendonly=true);

-- Setup a fault to ensure that both sessions pauses while creating an index,
-- ensuring a concurrent index creation.
SELECT gp_inject_fault('defineindex_before_acquire_lock', 'suspend', 1);

-- Attempt to concurrently create an index
1>: CREATE INDEX concurrent_index_test_idxa1 ON concurrent_index_test (value);
2>: CREATE INDEX concurrent_index_test_idxa2 ON concurrent_index_test (value);
SELECT gp_wait_until_triggered_fault('defineindex_before_acquire_lock', 2, 1);
SELECT gp_inject_fault('defineindex_before_acquire_lock', 'reset', 1);

-- Both index creation attempts should succeed
1<:
2<:

-- now test following concurrent index creations on AO also won't
-- deadlock
SELECT gp_inject_fault('defineindex_before_acquire_lock', 'suspend', 1);
1>: CREATE INDEX concurrent_index_test_idxb1 ON concurrent_index_test (value);
2>: CREATE INDEX concurrent_index_test_idxb2 ON concurrent_index_test (value);
SELECT gp_wait_until_triggered_fault('defineindex_before_acquire_lock', 2, 1);
SELECT gp_inject_fault('defineindex_before_acquire_lock', 'reset', 1);

-- Both index creation attempts should succeed
1<:
2<:
