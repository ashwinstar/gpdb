drop schema if exists dpe cascade;
create schema dpe;
set search_path='dpe';
set gp_segments_for_planner=2;

drop table if exists pt;
drop table if exists pt1;
drop table if exists t;
drop table if exists t1;

create table pt(dist int, pt1 text, pt2 text, pt3 text, ptid int) 
DISTRIBUTED BY (dist)
PARTITION BY RANGE(ptid) 
          (
          START (0) END (5) EVERY (1),
          DEFAULT PARTITION junk_data
          )
;

create table pt1(dist int, pt1 text, pt2 text, pt3 text, ptid int) 
DISTRIBUTED RANDOMLY
PARTITION BY RANGE(ptid) 
          (
          START (0) END (5) EVERY (1),
          DEFAULT PARTITION junk_data
          )
;

create table t(dist int, tid int, t1 text, t2 text);

create index pt1_idx on pt using btree (pt1);
create index ptid_idx on pt using btree (ptid);

insert into pt select i, 'hello' || i, 'world', 'drop this', i % 6 from generate_series(0,53) i;

insert into t select i, i % 6, 'hello' || i, 'bar' from generate_series(0,1) i;

create table t1 as select * from t;

insert into pt1 select * from pt;

analyze pt;
analyze pt1;
analyze t;
analyze t1;

--
-- Simple positive cases
--

explain select * from t, pt where tid = ptid;

select * from t, pt where tid = ptid;

explain select * from t, pt where tid + 1 = ptid;

select * from t, pt where tid + 1 = ptid;

explain select * from t, pt where tid = ptid and t1 = 'hello' || tid;

select * from t, pt where tid = ptid and t1 = 'hello' || tid;

explain select * from t, pt where t1 = pt1 and ptid = tid;

select * from t, pt where t1 = pt1 and ptid = tid;

--
-- in and exists clauses
--

explain select * from pt where ptid in (select tid from t where t1 = 'hello' || tid);

select * from pt where ptid in (select tid from t where t1 = 'hello' || tid);

explain select * from pt where exists (select 1 from t where tid = ptid and t1 = 'hello' || tid);

select * from pt where exists (select 1 from t where tid = ptid and t1 = 'hello' || tid);

--
-- group-by on top
--

explain select count(*) from t, pt where tid = ptid;

select count(*) from t, pt where tid = ptid;

--
-- window function on top
--

explain select *, rank() over (order by ptid,pt1) from t, pt where tid = ptid;

select *, rank() over (order by ptid,pt1) from t, pt where tid = ptid;

--
-- set ops
--

explain select * from t, pt where tid = ptid
	  union all
	  select * from t, pt where tid + 2 = ptid;

select * from t, pt where tid = ptid
	  union all
	  select * from t, pt where tid + 2 = ptid;

--
-- set-ops
--

explain select count(*) from
	( select * from t, pt where tid = ptid
	  union all
	  select * from t, pt where tid + 2 = ptid
	  ) foo;

select count(*) from
	( select * from t, pt where tid = ptid
	  union all
	  select * from t, pt where tid + 2 = ptid
	  ) foo;


--
-- other join types (NL)
--
set enable_hashjoin=off;
set enable_nestloop=on;
set enable_mergejoin=off;

explain select * from t, pt where tid = ptid;

select * from t, pt where tid = ptid;

set enable_hashjoin=on;
set enable_nestloop=off;
set enable_mergejoin=off;

--
-- index scan
--

set enable_seqscan=off;
set enable_indexscan=on;
set enable_bitmapscan=off;
set enable_hashjoin=off;

explain select * from t, pt where tid = ptid and pt1 = 'hello0';

select * from t, pt where tid = ptid and pt1 = 'hello0';

--
-- NL Index Scan
--
set enable_nestloop=on;
set enable_indexscan=on;
set enable_seqscan=off;
set enable_hashjoin=off;

explain select * from t, pt where tid = ptid and t1 = 'hello0';

select * from t, pt where tid = ptid and t1 = 'hello0';

--
-- Negative test cases where transform does not apply
--

set enable_indexscan=off;
set enable_seqscan=on;
set enable_hashjoin=on;
set enable_nestloop=off;

explain select * from t, pt where t1 = pt1;

select * from t, pt where t1 = pt1;

explain select * from t, pt where tid < ptid;

select * from t, pt where tid < ptid;

--
-- cascading joins
--

explain select * from t, t1, pt where t1.t2 = t.t2 and t1.tid = ptid and t.t1 = 'hello0';

select * from t, t1, pt where t1.t2 = t.t2 and t1.tid = ptid and t.t1 = 'hello0';


--
-- explain analyze
--

explain analyze select * from t, pt where tid = ptid;

