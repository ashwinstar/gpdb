-- Test Mark/Restore in Material Node
drop table if exists test1;
drop table if exists test2;
create table test1 (a integer);
create table test2 (a integer);
insert into test1 select a from generate_series(1,400000) a;
insert into test2 select a from generate_series(1,400000) a;

set enable_hashagg=off;
set enable_mergejoin=on;
set enable_hashjoin=off;
set enable_nestloop=off;
set statement_mem=10000;

select t1.*, t2.*
from (select a from test1 group by a) t1,
     (select a from test2 group by a) t2
where t1.a = t2.a;
-- Test Hash Aggregation when the work mem is too small for the hash table
drop table if exists test;
create table test (a integer, b integer);
insert into test select a, a%25 from generate_series(1,8000) a;
analyze;
set enable_hashagg=on;
set enable_groupagg=off;
set statement_mem=10000;

select b,count(*) from test group by b;

select b,count(*) from test group by b;
-- Test Hash Join when the work mem is too small for the hash table
drop table if exists test;
create table test (a integer, b integer);
insert into test select a, a%25 from generate_series(1,800000) a;
analyze; -- We have to do an analyze to force a hash join
set enable_mergejoin=off;
set enable_nestloop=off;
set enable_hashjoin=on;
set statement_mem=10000;

select t1.* from test t1, test t2 where t1.a = t2.a;
