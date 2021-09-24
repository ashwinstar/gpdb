-- Test to validate bitmap index is fine after crash recovery in-case
-- meta-page buffer eviction happens. There used to be bug if the
-- metapage is not present in shared buffers, it will not be fetched
-- from disk. Instead, a zeroed out page will be returned. A
-- subsequent flush of the metapage will lead to an inadvertent
-- overwrite.
include: helpers/server_helpers.sql;

-- 1:CREATE EXTENSION IF NOT EXISTS gp_inject_fault;
-- -- skip FTS probes for this test to avoid segment being marked down on restart
-- 1:SELECT gp_inject_fault_infinite('fts_probe', 'skip', dbid)
--     FROM gp_segment_configuration WHERE role='p' AND content=-1;
-- 1:SELECT gp_request_fts_probe_scan();
-- 1:SELECT gp_wait_until_triggered_fault('fts_probe', 1, dbid)
--     FROM gp_segment_configuration WHERE role='p' AND content=-1;

-- -- test setup
-- 1:CREATE TABLE bm(a int);
-- 1:CREATE INDEX ON bm USING bitmap (a);
-- -- pause checkpoint to make sure CRASH RECOVERY happens for bitmap index replay
-- 1:SELECT gp_inject_fault_infinite('checkpoint', 'skip', dbid) FROM gp_segment_configuration WHERE role='p';
-- 1:CHECKPOINT;

-- -- this insert's WAL we wish to replay
-- 1:insert into bm select generate_series(1, 5000);

-- -- set small shared_buffers to make sure META_PAGE of bitmap index evicts out
-- 1U: ALTER SYSTEM set shared_buffers to 20;
-- 1:SELECT pg_ctl(datadir, 'restart') from gp_segment_configuration where role = 'p' and content = 1;

-- -- force index scan and make sure the index is fine
-- 2:SET enable_seqscan to off;
-- 2:SELECT * FROM bm WHERE a < 10;

-- -- teardown cleanup for the test
-- 1Uq:
-- 1U:ALTER SYSTEM reset shared_buffers;
-- 2:SELECT pg_ctl(datadir, 'restart') from gp_segment_configuration where role = 'p' and content = 1;
-- 3:SELECT gp_inject_fault_infinite('checkpoint', 'reset', dbid) FROM gp_segment_configuration WHERE role='p';

-- 3:SELECT gp_inject_fault('fts_probe', 'reset', dbid) FROM gp_segment_configuration WHERE role='p' AND content=-1;


-- Setup fault injectors
CREATE EXTENSION IF NOT EXISTS gp_inject_fault;

--select gp_inject_fault_infinite('checkpoint_dtx_info', 'suspend', dbid) from gp_segment_configuration;
-- Here we use append optimized table to speed up create full bitmap pages
-- since each transaction use different seg file number. And ao table's AOTupleId
-- is composed of (seg file number, row number). So this will lead to lots of
-- compressed words in the first bitmap page.
-- With the below transacions in each session, on seg0, the bitmap for id=97
-- will generate two bitmap pages, and the first page is a full page.
-- Use heap table, delete tuples and then vacuum should be the same. But it needs huge tuples.
CREATE TABLE bmupdate (id int) with(appendonly = true) DISTRIBUTED BY (id);

1: begin;
2: begin;
3: begin;
4: begin;
5: begin;
6: begin;
7: begin;
8: begin;
9: begin;
10: begin;
11: begin;
12: begin;
13: begin;
14: begin;
15: begin;
16: begin;
17: begin;
18: begin;
19: begin;
20: begin;
21: begin;
22: begin;

1: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
2: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
3: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
4: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
5: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
6: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
7: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
8: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
9: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
10: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
11: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
12: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
13: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
14: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
15: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
16: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
17: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
18: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
19: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
20: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
21: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;
22: INSERT INTO bmupdate SELECT i%10000 FROM generate_series(1, 1000000) AS i;

1: commit;
2: commit;
3: commit;
4: commit;
5: commit;
6: commit;
7: commit;
8: commit;
9: commit;
10: commit;
11: commit;
12: commit;
13: commit;
14: commit;
15: commit;
16: commit;
17: commit;
18: commit;
19: commit;
20: commit;
21: commit;
22: commit;

CREATE INDEX idx_bmupdate__id ON bmupdate USING bitmap (id);
-- pause checkpoint to make sure CRASH RECOVERY happens for bitmap index replay
1:SELECT gp_inject_fault_infinite('checkpoint', 'skip', dbid) FROM gp_segment_configuration WHERE role='p';
1:CHECKPOINT;

--1: begin;
--2: begin;
1: INSERT INTO bmupdate VALUES (97);  -- fetch a smaller seg file number
--2: INSERT INTO bmupdate VALUES (98);  -- fetch a larger segfile number and trigger updatesetbit_inpage(), it will generate a new bitmap page(nextPage) to store extra bitmaps from the current page(bitmapPage) since the current page is a full page, it will expand compressed word and generate new words.

--2: INSERT INTO bmupdate VALUES (97);  -- touch the new buffer again to avoid full page write for xlog.
1: INSERT INTO bmupdate VALUES (97);  -- use the small segfile to generate a smaller AOTupleID before new bitmap page (nextPage) in updatesetbit_inpage().
-- Then please verify the md5 for the index file between the primary and mirror after `gpstop -a`.
