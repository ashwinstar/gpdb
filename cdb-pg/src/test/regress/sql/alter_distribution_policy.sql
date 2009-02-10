-- ALTER TABLE ... SET DISTRIBUTED BY
-- This is the main interface for system expansion

\set DATA values(1, 2), (2, 3), (3, 4)
-- Basic sanity tests
create table atsdb (i int, j text) distributed by (i);
insert into atsdb :DATA;

-- should fail
alter table atsdb set distributed by ();
alter table atsdb set distributed by (m);
alter table atsdb set distributed by (i, i);
alter table atsdb set distributed by (i, m);
alter table atsdb set distributed by (i);

-- should work
alter table atsdb set distributed randomly;
select localoid::regclass, attrnums from gp_distribution_policy where localoid = 'atsdb'::regclass;
-- not possible to correctly verify random distribution

alter table atsdb set distributed by (j);
select localoid::regclass, attrnums from gp_distribution_policy where localoid = 'atsdb'::regclass;
-- verify that the data is correctly redistributed by building a fresh 
-- table with the same policy
create table ats_test (i int, j text) distributed by (j);
insert into ats_test :DATA;
select gp_segment_id, * from ats_test except
select gp_segment_id, * from atsdb;
drop table ats_test;

alter table atsdb set distributed by (i, j);
select localoid::regclass, attrnums from gp_distribution_policy where localoid = 'atsdb'::regclass;
-- verify
create table ats_test (i int, j text) distributed by (i, j);
insert into ats_test :DATA;
select gp_segment_id, * from ats_test except
select gp_segment_id, * from atsdb;
drop table ats_test;

alter table atsdb set distributed by (j, i);
select localoid::regclass, attrnums from gp_distribution_policy where localoid = 'atsdb'::regclass;
-- verify
create table ats_test (i int, j text) distributed by (j, i);
insert into ats_test :DATA;
select gp_segment_id, * from ats_test except
select gp_segment_id, * from atsdb;
drop table ats_test;

-- Now make sure indexes work.
create index atsdb_i_idx on atsdb(i);
set enable_seqscan to off;
explain select * from atsdb where i = 1;
select * from atsdb where i = 1;
alter table atsdb set distributed by (i);
explain select * from atsdb where i = 1;
select * from atsdb where i = 1;

drop table atsdb;

-- Now try AO
create table atsdb_ao (i int, j text) distributed by (i);
insert into atsdb_ao select i, (i+1)::text from generate_series(1, 100) i;
insert into atsdb_ao select i, (i+1)::text from generate_series(1, 100) i;
-- check that we're an AO table
explain select count(*) from atsdb_ao;
select count(*) from atsdb_ao;
alter table atsdb_ao set distributed by (j);
-- Still AO?
explain select count(*) from atsdb_ao;
select count(*) from atsdb_ao;
drop table atsdb_ao;

-- Check divergent distribution policies for partitioning.
create table atsdb (i int, j int, k int) partition by range(k)
(start(1) end(4) every(1));
alter table atsdb_1_prt_2 set distributed by (j);
alter table atsdb_1_prt_3 set distributed randomly;
-- test COPY
copy atsdb from stdin delimiter '|';
1|1|1
2|1|1
2|2|1
3|1|1
3|2|1
3|3|1
1|1|2
2|1|2
2|2|2
3|1|2
3|2|2
3|3|2
1|1|3
2|1|3
2|2|3
3|1|3
3|2|3
3|3|3
\.

select count(*) from atsdb;

-- compare distribution: we create a table, identical to the partitioned table,
-- and compare how the tuples have been distributed.
create table atsdb_1 (like atsdb_1_prt_1);
copy atsdb_1 from stdin delimiter '|';
1|1|1
2|1|1
2|2|1
3|1|1
3|2|1
3|3|1
\.
select gp_segment_id, * from atsdb where k = 1 except 
select gp_segment_id, * from atsdb_1;

create table atsdb_2 (like atsdb_1_prt_2);
copy atsdb_2 from stdin delimiter '|';
1|1|2
2|1|2
2|2|2
3|1|2
3|2|2
3|3|2
\.
select gp_segment_id, * from atsdb where k = 2 except
select gp_segment_id, * from atsdb_2;

-- Can't test randomly distributed

-- Can't test INSERT (yet)

drop table atsdb, atsdb_1, atsdb_2;
