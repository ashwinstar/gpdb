--
-- ORCA tests
--

-- master only tables

create schema orca;

create table orca.r();
alter table orca.r add column a int;
alter table orca.r add column b int;

insert into orca.r select i, i/3 from generate_series(1,20) i;

create table orca.s();
alter table orca.s add column c int;
alter table orca.s add column d int;

insert into orca.s select i, i/2 from generate_series(1,30) i;

set gp_log_optimizer=on;
set gp_optimizer=on;
----------------------------------------------------------------------

select * from orca.r;
select * from orca.r, orca.s where r.a=s.c;
select * from orca.r, orca.s where r.a<s.c+1 or r.a>s.c;
select sum(r.a) from orca.r;
select count(*) from orca.r;
select a, b from orca.r, orca.s group by a,b;
select r.a+1 from orca.r;
select * from orca.r, orca.s where r.a<s.c or (r.b<s.d and r.b>s.c);
select case when r.a<s.c then r.a<s.c else r.a<s.c end from orca.r, orca.s;
select case r.b<s.c when true then r.b else s.c end from orca.r, orca.s where r.a = s.d;
select * from orca.r limit 100;
select * from orca.r limit 10 offset 9;
select * from orca.r offset 10;
select sqrt(r.a) from orca.r;
select pow(r.b,r.a) from orca.r;
select b from orca.r group by b having  count(*) > 2;
select b from orca.r group by b having  count(*) <= avg(a) + (select count(*) from orca.s where s.c = r.b);
select sum(a) from orca.r group by b having count(*) > 2 order by b+1;
select sum(a) from orca.r group by b having count(*) > 2 order by b+1;

-- constants

select 0.001::numeric from orca.r;
select NULL::text, NULL::int from orca.r;
select 'helloworld'::text, 'helloworld2'::varchar from orca.r;
select 129::bigint, 5623::int, 45::smallint from orca.r;

select 0.001::numeric from orca.r;
select NULL::text, NULL::int from orca.r;
select 'helloworld'::text, 'helloworld2'::varchar from orca.r;
select 129::bigint, 5623::int, 45::smallint from orca.r;

--  distributed tables

----------------------------------------------------------------------
set gp_optimizer=off;

create table orca.foo (x1 int, x2 int, x3 int);
create table orca.bar1 (x1 int, x2 int, x3 int) distributed randomly;
create table orca.bar2 (x1 int, x2 int, x3 int) distributed randomly;

insert into orca.foo select i,i+1,i+2 from generate_series(1,10) i;

insert into orca.bar1 select i,i+1,i+2 from generate_series(1,20) i;
insert into orca.bar2 select i,i+1,i+2 from generate_series(1,30) i;

set gp_optimizer=on;
----------------------------------------------------------------------

-- produces result node

select x2 from orca.foo where x1 in (select x2 from orca.bar1);
select 1;

----------------------------------------------------------------------
set gp_optimizer=off;

drop table orca.r cascade;
create table orca.r(a int, b int);
create unique index r_a on orca.r(a);
create index r_b on orca.r(b);

insert into orca.r select i, i%3 from generate_series(1,20) i;

drop table orca.s;
create table orca.s(c int, d int);

insert into orca.s select i%7, i%2 from generate_series(1,30) i;

analyze orca.r;
analyze orca.s;

set gp_optimizer=on;
----------------------------------------------------------------------

select * from orca.r, orca.s where r.a=s.c;

-- Materialize node
select * from orca.r, orca.s where r.a<s.c+1 or r.a>s.c;

-- empty target list
select r.* from orca.r, orca.s where s.c=2;

----------------------------------------------------------------------
set gp_optimizer=off;

create table orca.m();
alter table orca.m add column a int;
alter table orca.m add column b int;

create table orca.m1();
alter table orca.m1 add column a int;
alter table orca.m1 add column b int;

insert into orca.m select i-1, i%2 from generate_series(1,35) i;
insert into orca.m1 select i-2, i%3 from generate_series(1,25) i;

insert into orca.r values (null, 1);

set gp_optimizer=on;
----------------------------------------------------------------------

-- join types
select r.a, s.c from orca.r left outer join orca.s on(r.a=s.c);
select r.a, s.c from orca.r right outer join orca.s on(r.a=s.c);
select * from orca.r where exists (select * from orca.s where s.c=r.a + 2);
select * from orca.r where exists (select * from orca.s where s.c=r.b);
select * from orca.m where m.a not in (select a from orca.m1 where a=5);
select * from orca.m where m.a not in (select a from orca.m1);
select * from orca.m where m.a in (select a from orca.m1 where m1.a-1 = m.b);

-- enable_hashjoin=off; enable_mergejoin=on
select 1 from orca.m, orca.m1 where m.a = m1.a and m.b!=m1.b;

-- plan.qual vs hashclauses/join quals:
select * from orca.r left outer join orca.s on (r.a=s.c and r.b<s.d) where s.d is null;
select * from orca.r m full outer join orca.r m1 on (m.a=m1.a) where m.a is null;

-- sort
select * from orca.r join orca.s on(r.a=s.c) order by r.a, s.d;
select * from orca.r join orca.s on(r.a=s.c) order by r.a, s.d limit 10;
select * from orca.r join orca.s on(r.a=s.c) order by r.a + 5, s.d limit 10;

-- group by
select 1 from orca.m group by a+b;

-- const table get

select * from orca.r where a = (select 1);

----------------------------------------------------------------------
set gp_optimizer=off;

insert into orca.m values (1,-1), (1,2), (1,1);

set gp_optimizer=on;
----------------------------------------------------------------------

-- computed columns
select a,a,a+b from orca.m;
select a,a+b,a+b from orca.m;

-- func expr
select * from orca.m where a=abs(b);

-- grouping sets

select a,b,count(*) from orca.m group by grouping sets ((a), (a,b));
select b,count(*) from orca.m group by grouping sets ((a), (a,b));
select a,count(*) from orca.m group by grouping sets ((a), (a,b));
select a,count(*) from orca.m group by grouping sets ((a), (b));
select a,b,count(*) from orca.m group by rollup(a, b);
select count(*) from orca.m group by ();
select a, count(*) from orca.r group by (), a;
select a, count(*) from orca.r group by grouping sets ((),(a));

-- arrays
select array[array[a,b]], array[b] from orca.r;

-- setops
select a, b from m union select b,a from orca.m;
SELECT a from m UNION ALL select b from orca.m UNION ALL select a+b from orca.m group by 1;

----------------------------------------------------------------------
set gp_optimizer=off;

drop table if exists orca.foo;
create table orca.foo(a int, b int, c int, d int);

drop table if exists orca.bar;
create table orca.bar(a int, b int, c int);

insert into orca.foo select i, i%2, i%4, i-1 from generate_series(1,40)i;
insert into orca.bar select i, i%3, i%2 from generate_series(1,30)i;

set gp_optimizer=on;
----------------------------------------------------------------------

--- distinct operation
SELECT distinct a, b from orca.foo;
SELECT distinct foo.a, bar.b from orca.foo, orca.bar where foo.b = bar.a;
SELECT distinct a, b from orca.foo;
SELECT distinct a, count(*) from orca.foo group by a;
SELECT distinct foo.a, bar.b, sum(bar.c+foo.c) from orca.foo, orca.bar where foo.b = bar.a group by foo.a, bar.b;
SELECT distinct a, count(*) from orca.foo group by a;
SELECT distinct foo.a, bar.b from orca.foo, orca.bar where foo.b = bar.a;
SELECT distinct foo.a, bar.b, sum(bar.c+foo.c) from orca.foo, orca.bar where foo.b = bar.a group by foo.a, bar.b;

SELECT distinct a, b from orca.foo;
SELECT distinct a, count(*) from orca.foo group by a;
SELECT distinct foo.a, bar.b from orca.foo, orca.bar where foo.b = bar.a;
SELECT distinct foo.a, bar.b, sum(bar.c+foo.c) from orca.foo, orca.bar where foo.b = bar.a group by foo.a, bar.b;

--- window operations
select row_number() over() from orca.foo order by 1;
select rank() over(partition by b order by count(*)/sum(a)) from orca.foo group by a, b order by 1;
select row_number() over(order by foo.a) from orca.foo inner join orca.bar using(b) group by foo.a, bar.b, bar.a;
select 1+row_number() over(order by foo.a+bar.a) from orca.foo inner join orca.bar using(b);
select row_number() over(order by foo.a+ bar.a)/count(*) from orca.foo inner join orca.bar using(b) group by foo.a, bar.a, bar.b;
select count(*) over(partition by b order by a range between 1 preceding and (select count(*) from orca.bar) following) from orca.foo;
select a+1, rank() over(partition by b+1 order by a+1) from orca.foo order by 1, 2;

--- cte
with x as (select a, b from orca.r)
select rank() over(partition by a, case when b = 0 then a+b end order by b asc) as rank_within_parent from x order by a desc ,case when a+b = 0 then a end ,b;

with v1 as (select a from orca.r), v2 as (select x.a as a1, y.a as a2 from v1 x, v1 y) select * from v2 where a1 = 10 order by 1, 2;

-- clean up
drop schema orca cascade;