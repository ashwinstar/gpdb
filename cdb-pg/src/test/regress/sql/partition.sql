set enable_partition_rules = false;
set gp_enable_hash_partitioned_tables = true;

-- missing subpartition by
create table ggg (a char(1), b char(2), d char(3))
distributed by (a)
partition by hash (b)
(
partition aa (subpartition cc, subpartition dd),
partition bb (subpartition cc, subpartition dd)
);

-- missing subpartition spec
create table ggg (a char(1), b char(2), d char(3))
distributed by (a)
partition by hash (b)
subpartition by hash (d) 
(
partition aa ,
partition bb 
);

-- subpart spec conflict
create table ggg (a char(1), b char(2), d char(3))
distributed by (a)
partition by hash (b) 
subpartition by hash (d) subpartition template (subpartition jjj)
(
partition aa (subpartition cc, subpartition dd),
partition bb (subpartition cc, subpartition dd)
);

-- missing subpartition by
create table ggg (a char(1), b char(2), d char(3))
distributed by (a)
partition by hash (b)
subpartition by hash (d)
(
partition aa (subpartition cc, subpartition dd (subpartition iii)),
partition bb (subpartition cc, subpartition dd)
);

-- Test column lookup works
create table ggg (a char(1), b char(2), d char(3))
distributed by (a)
partition by hash(doesnotexist)
partitions 3;

create table ggg (a char(1), b char(2), d char(3))
distributed by (a)
partition by hash(b)
partitions 3
subpartition by list(alsodoesntexist)
subpartition template (
subpartition aa values(1)
);

-- should work
create table ggg (a char(1), b char(2), d char(3))
distributed by (a)
partition by hash (b)
subpartition by hash (d) 
(
partition aa (subpartition cc, subpartition dd),
partition bb (subpartition cc, subpartition dd)
);

drop table ggg cascade;

-- disable hash partitions
set gp_enable_hash_partitioned_tables = false;

create table ggg (a char(1), b char(2), d char(3))
distributed by (a)
partition by hash (b)
subpartition by hash (d) 
(
partition aa (subpartition cc, subpartition dd),
partition bb (subpartition cc, subpartition dd)
);

drop table ggg cascade;

set gp_enable_hash_partitioned_tables = true;

-- should work
create table ggg (a char(1), b char(2), d char(3), e int)
distributed by (a)
partition by hash (b)
subpartition by hash (d) 
subpartition template ( 
subpartition cc,
subpartition dd
), 
subpartition by hash (e) 
subpartition template ( 
subpartition ee,
subpartition ff
) 
(
partition aa,
partition bb
);

drop table ggg cascade;

-- should work
create table ggg (a char(1), b char(2), d char(3), e int)
distributed by (a)
partition by hash (b)
subpartition by hash (d),
subpartition by hash (e)
subpartition template ( 
subpartition ee,
subpartition ff
) 
(
partition aa (subpartition cc, subpartition dd),
partition bb (subpartition cc, subpartition dd)
);

drop table ggg cascade;

-- doesn't work because cannot have nested declaration in template
create table ggg (a char(1), b char(2), d char(3), e int)
distributed by (a)
partition by hash (b)
subpartition by hash (d) 
subpartition template ( 
subpartition cc (subpartition ee, subpartition ff),
subpartition dd (subpartition ee, subpartition ff)
), 
subpartition by hash (e) 
(
partition aa,
partition bb
);

drop table ggg cascade;

--ERROR: Missing boundary specification in partition 'aa' of type LIST 
create table fff (a char(1), b char(2), d char(3)) distributed by
(a) partition by list (b) (partition aa ); 


-- ERROR: Invalid use of RANGE boundary specification in partition
--   number 1 of type LIST
create table fff (a char(1), b char(2), d char(3)) distributed by (a)
partition by list (b) (start with ('a') );


-- should work
create table fff (a char(1), b char(2), d char(3)) distributed by (a)
partition by list (b) (partition aa values ('2'));

drop table fff cascade;

-- Invalid use of RANGE boundary specification in partition "cc" of 
-- type HASH (at depth 2)
create table ggg (a char(1), b char(2), d char(3), e int) distributed by (a)
partition by hash (b) subpartition by hash (d),
subpartition by hash (e) 
subpartition template ( subpartition ee, subpartition ff ) (
partition aa (subpartition cc, subpartition dd), partition bb
(subpartition cc start with ('a') , subpartition dd) );

-- this is subtly wrong -- it defines 4 partitions
-- the problem is the comma before "end", which causes us to
-- generate 2 anonymous partitions.
-- This is an error: 
-- ERROR:  invalid use of mixed named and unnamed RANGE boundary specifications
create table ggg (a char(1), b int, d char(3))
distributed by (a)
partition by range (b)
(
partition aa start ('2007'), end ('2008'),
partition bb start ('2008'), end ('2009')
);

create table ggg (a char(1), b int)
distributed by (a)
partition by range(b)
(
partition aa start ('2007'), end ('2008')
);

drop table ggg cascade;

create table ggg (a char(1), b date, d char(3))
distributed by (a)
partition by range (b)
(
partition aa start (date '2007-01-01') end (date '2008-01-01'),
partition bb start (date '2008-01-01') end (date '2009-01-01')
);


drop table ggg cascade;

-- don't allow nonconstant expressions, even simple ones...
create table ggg (a char(1), b numeric, d numeric)
distributed by (a)
partition by range (b,d)
(
partition aa start (2007,1) end (2008,2+2),
partition bb start (2008,2) end (2009,3)
);

-- should work
create table ggg (a char(1), b numeric, d numeric)
distributed by (a)
partition by range (b,d)
(
partition aa start (2007,1) end (2008,2),
partition bb start (2008,2) end (2009,3)
);


drop table ggg cascade;

-- demo starts here

-- nested subpartitions
create table ggg
 (a char(1),   b date,
  d char(3),  e numeric,
  f numeric,  g numeric,
  h numeric)
distributed by (a)
partition by hash(b)
partitions 2
subpartition by hash(d)
subpartitions 2,
subpartition by hash(e) subpartitions 2,
subpartition by hash(f) subpartitions 2,
subpartition by hash(g) subpartitions 2,
subpartition by hash(h) subpartitions 2;

drop table ggg cascade;


-- named, inline subpartitions
create table ggg (a char(1), b char(2), d char(3))
distributed by (a)
partition by hash (b)
subpartition by hash (d) 
(
partition aa (subpartition cc, subpartition dd),
partition bb (subpartition cc, subpartition dd)
);

drop table ggg cascade;


-- subpartitions with templates
create table ggg (a char(1), b char(2), d char(3), e numeric)
distributed by (a)
partition by hash (b)
subpartition by hash (d) 
subpartition template ( 
subpartition cc,
subpartition dd
), 
subpartition by hash (e) 
subpartition template ( 
subpartition ee,
subpartition ff
) 
(
partition aa,
partition bb
);

drop table ggg cascade;


-- mixed inline subpartition declarations with templates
create table ggg (a char(1), b char(2), d char(3), e numeric)
distributed by (a)
partition by hash (b)
subpartition by hash (d) , 
subpartition by hash (e) 
subpartition template ( 
subpartition ee,
subpartition ff
) 
(
partition aa (subpartition cc, subpartition dd),
partition bb (subpartition cc, subpartition dd)
);

drop table ggg cascade;


-- basic list partition
create table ggg (a char(1), b char(2), d char(3))
distributed by (a)
partition by LIST (b)
(
partition aa values ('a', 'b', 'c', 'd'),
partition bb values ('e', 'f', 'g')
);

insert into ggg values ('x', 'a');
insert into ggg values ('x', 'b');
insert into ggg values ('x', 'c');
insert into ggg values ('x', 'd');
insert into ggg values ('x', 'e');
insert into ggg values ('x', 'f');
insert into ggg values ('x', 'g');
insert into ggg values ('x', 'a');
insert into ggg values ('x', 'b');
insert into ggg values ('x', 'c');
insert into ggg values ('x', 'd');
insert into ggg values ('x', 'e');
insert into ggg values ('x', 'f');
insert into ggg values ('x', 'g');

select * from ggg order by 1, 2;

-- not ready
select * from ggg_1_prt_aa 1, 2;
select * from ggg_1_prt_bb 1, 2;

drop table ggg cascade;

-- documentation example - partition by list and range
CREATE TABLE rank (id int, rank int, year date, gender 
char(1)) DISTRIBUTED BY (id, gender, year)
partition by list (gender)
subpartition by range (year)
subpartition template (
start (date '2001-01-01'),
start (date '2002-01-01'),
start (date '2003-01-01'),
start (date '2004-01-01'),
start (date '2005-01-01')
)
(
  partition boys values ('M'),
  partition girls values ('F')
);

insert into rank values (1, 1, date '2001-01-15', 'M');
insert into rank values (2, 1, date '2002-02-15', 'M');
insert into rank values (3, 1, date '2003-03-15', 'M');
insert into rank values (4, 1, date '2004-04-15', 'M');
insert into rank values (5, 1, date '2005-05-15', 'M');
insert into rank values (6, 1, date '2001-01-15', 'F');
insert into rank values (7, 1, date '2002-02-15', 'F');
insert into rank values (8, 1, date '2003-03-15', 'F');
insert into rank values (9, 1, date '2004-04-15', 'F');
insert into rank values (10, 1, date '2005-05-15', 'F');

select * from rank order by 1, 2, 3, 4;
select * from rank_1_prt_boys order by 1, 2, 3, 4;
select * from rank_1_prt_girls order by 1, 2, 3, 4;
select * from rank_1_prt_girls_2_prt_1 order by 1, 2, 3, 4;
select * from rank_1_prt_girls_2_prt_2 order by 1, 2, 3, 4;


drop table rank cascade;



-- range list hash combo
create table ggg (a char(1), b date, d char(3), e numeric)
distributed by (a)
partition by range (b)
subpartition by list(d),
subpartition by hash(e) subpartitions 3
(
partition aa 
start  (date '2007-01-01') 
end (date '2008-01-01') 
       (subpartition dd values ('1', '2', '3'), 
	    subpartition ee values ('4', '5', '6')),
partition bb
start  (date '2008-01-01') 
end (date '2009-01-01') 
       (subpartition dd values ('1', '2', '3'),
	    subpartition ee values ('4', '5', '6'))
);

drop table ggg cascade;


-- demo ends here


-- LIST validation

-- duplicate partition name
CREATE TABLE rank (id int, rank int, year date, gender
char(1)) DISTRIBUTED BY (id, gender, year)
partition by list (gender)
(
  partition boys values ('M'),
  partition girls values ('a'),
  partition girls values ('b'),
  partition girls values ('c'),
  partition girls values ('d'),
  partition girls values ('e'),
  partition bob values ('M')
);

-- duplicate values
CREATE TABLE rank (id int, rank int, year date, gender
char(1)) DISTRIBUTED BY (id, gender, year)
partition by list (rank,gender)
(
 values ((1, 'M')),
 values ((2, 'M')),
 values ((3, 'M')),
 values ((1, 'F')),
 partition ff values ((4, 'M')),
 partition bb values ((1, 'M'))
);


-- RANGE validation

-- legal if end of aa not inclusive
create table ggg (a char(1), b date, d char(3)) 
distributed by (a)
partition by range (b)
(
partition aa start (date '2007-01-01') end (date '2008-01-01'),
partition bb start (date '2008-01-01') end (date '2009-01-01') 
every (interval '10 days'));

drop table ggg cascade;


-- bad - legal if end of aa not inclusive
create table ggg (a char(1), b date, d char(3)) 
distributed by (a)
partition by range (b)
(
partition aa start (date '2007-01-01') end (date '2008-01-01') inclusive,
partition bb start (date '2008-01-01') end (date '2009-01-01') 
every (interval '10 days'));

drop table ggg cascade;

-- legal because start of bb not inclusive
create table ggg (a char(1), b date, d char(3)) 
distributed by (a)
partition by range (b)
(
partition aa start (date '2007-01-01') end (date '2008-01-01') inclusive,
partition bb start (date '2008-01-01') exclusive end (date '2009-01-01') 
every (interval '10 days'));

drop table ggg cascade;

-- legal if end of aa not inclusive
create table ggg (a char(1), b date, d char(3)) 
distributed by (a)
partition by range (b)
(
partition bb start (date '2008-01-01') end (date '2009-01-01'),
partition aa start (date '2007-01-01') end (date '2008-01-01')
);

drop table ggg cascade;

-- bad - legal if end of aa not inclusive
create table ggg (a char(1), b date, d char(3)) 
distributed by (a)
partition by range (b)
(
partition bb start (date '2008-01-01') end (date '2009-01-01'),
partition aa start (date '2007-01-01') end (date '2008-01-01') inclusive
);

drop table ggg cascade;

-- legal because start of bb not inclusive
create table ggg (a char(1), b date, d char(3)) 
distributed by (a)
partition by range (b)
(
partition bb start (date '2008-01-01') exclusive end (date '2009-01-01'),
partition aa start (date '2007-01-01') end (date '2008-01-01') inclusive
);

drop table ggg cascade;

-- validate aa - start greater than end
create table ggg (a char(1), b date, d char(3)) 
distributed by (a)
partition by range (b)
(
partition bb start (date '2008-01-01') end (date '2009-01-01'),
partition aa start (date '2007-01-01') end (date '2006-01-01')
);

drop table ggg cascade;

-- formerly we could not set end of first partition because next is before
-- but we can sort them now so this is legal.
create table ggg (a char(1), b date, d char(3)) 
distributed by (a)
partition by range (b)
(
partition bb start (date '2008-01-01') ,
partition aa start (date '2007-01-01') 
);

drop table ggg cascade;

-- test cross type coercion
-- int -> char(N)
create table ggg (i int, a char(1))
distributed by (i)
partition by list(a)
(partition aa values(1, 2));

drop table ggg cascade;

-- int -> numeric
create table ggg (i int, n numeric(20, 2))
distributed by (i)
partition by list(n)
(partition aa values(1.22, 4.1));
drop table ggg cascade;

-- EVERY

--  the documentation example, rewritten with EVERY in a template
CREATE TABLE rank (id int,
rank int, year date, gender char(1))
DISTRIBUTED BY (id, gender, year)
partition by list (gender)
subpartition by range (year)
subpartition template (
start (date '2001-01-01')
end (date '2006-01-01') every (interval '1 year')) (
partition boys values ('M'),
partition girls values ('F')
);


insert into rank values (1, 1, date '2001-01-15', 'M');
insert into rank values (2, 1, date '2002-02-15', 'M');
insert into rank values (3, 1, date '2003-03-15', 'M');
insert into rank values (4, 1, date '2004-04-15', 'M');
insert into rank values (5, 1, date '2005-05-15', 'M');
insert into rank values (6, 1, date '2001-01-15', 'F');
insert into rank values (7, 1, date '2002-02-15', 'F');
insert into rank values (8, 1, date '2003-03-15', 'F');
insert into rank values (9, 1, date '2004-04-15', 'F');
insert into rank values (10, 1, date '2005-05-15', 'F');


select * from rank order by 1, 2, 3, 4;
select * from rank_1_prt_boys order by 1, 2, 3, 4;
select * from rank_1_prt_girls order by 1, 2, 3, 4;
select * from rank_1_prt_girls_2_prt_1 order by 1, 2, 3, 4;
select * from rank_1_prt_girls_2_prt_2 order by 1, 2, 3, 4;

drop table rank cascade;

-- integer ranges work too
create table ggg (id integer, a integer)
distributed by (id)
partition by range (a)
(start (1) end (10) every (1));

insert into ggg values (1, 1);
insert into ggg values (2, 2);
insert into ggg values (3, 3);
insert into ggg values (4, 4);
insert into ggg values (5, 5);
insert into ggg values (6, 6);
insert into ggg values (7, 7);
insert into ggg values (8, 8);
insert into ggg values (9, 9);
insert into ggg values (10, 10);

select * from ggg order by 1, 2;

select * from ggg_1_prt_1 order by 1, 2;
select * from ggg_1_prt_2 order by 1, 2;
select * from ggg_1_prt_3 order by 1, 2;
select * from ggg_1_prt_4 order by 1, 2;

drop table ggg cascade;

-- hash tests

create table ggg (a char(1), b varchar(2), d varchar(2))
distributed by (a)
partition by hash(b)
partitions 3
(partition a, partition b, partition c);

insert into ggg values (1,1,1);
insert into ggg values (2,2,1);
insert into ggg values (1,3,1);
insert into ggg values (2,2,3);
insert into ggg values (1,4,5);
insert into ggg values (2,2,4);
insert into ggg values (1,5,6);
insert into ggg values (2,7,3);
insert into ggg values (1,'a','b');
insert into ggg values (2,'c','c');

select * from ggg order by 1, 2, 3;

select * from ggg_1_prt_a order by 1, 2, 3;
select * from ggg_1_prt_b order by 1, 2, 3;
select * from ggg_1_prt_c order by 1, 2, 3;

drop table ggg cascade;

-- use multiple cols
create table ggg (a char(1), b varchar(2), d varchar(2))
distributed by (a)
partition by hash(b,d)
partitions 3
(partition a, partition b, partition c);

insert into ggg values (1,1,1);
insert into ggg values (2,2,1);
insert into ggg values (1,3,1);
insert into ggg values (2,2,3);
insert into ggg values (1,4,5);
insert into ggg values (2,2,4);
insert into ggg values (1,5,6);
insert into ggg values (2,7,3);
insert into ggg values (1,'a','b');
insert into ggg values (2,'c','c');

select * from ggg order by 1, 2, 3;

select * from ggg_1_prt_a order by 1, 2, 3;
select * from ggg_1_prt_b order by 1, 2, 3;
select * from ggg_1_prt_c order by 1, 2, 3;

drop table ggg cascade;

-- use multiple cols of different types and without a partition spec
create table ggg (a char(1), b varchar(2), d integer, e date)
distributed by (a)
partition by hash(b,d,e)
partitions 3;

insert into ggg values (1,1,1,date '2001-01-15');
insert into ggg values (2,2,1,date '2001-01-15');
insert into ggg values (1,3,1,date '2001-01-15');
insert into ggg values (2,2,3,date '2001-01-15');
insert into ggg values (1,4,5,date '2001-01-15');
insert into ggg values (2,2,4,date '2001-01-15');
insert into ggg values (1,5,6,date '2001-01-15');
insert into ggg values (2,7,3,date '2001-01-15');
insert into ggg values (1,'a',33,date '2001-01-15');
insert into ggg values (2,'c',44,date '2001-01-15');

select * from ggg order by 1, 2, 3, 4;

select * from ggg_1_prt_1 order by 1, 2, 3, 4;
select * from ggg_1_prt_2 order by 1, 2, 3, 4;
select * from ggg_1_prt_3 order by 1, 2, 3, 4;

drop table ggg cascade;

-- append only tests
create table foz (i int, d date) with (appendonly = true) distributed by (i)
partition by range (d) (start (date '2001-01-01') end (date '2005-01-01')
every(interval '1 year'));
insert into foz select i, '2001-01-01'::date + ('1 day'::interval * i) from
generate_series(1, 1000) i;
select count(*) from foz;
select count(*) from foz_1_prt_1;

select min(d), max(d) from foz;
select min(d), max(d) from foz_1_prt_1;
select min(d), max(d) from foz_1_prt_2;
select min(d), max(d) from foz_1_prt_3;
select min(d), max(d) from foz_1_prt_4;


drop table foz cascade;

-- copy test
create table foz (i int, d date) distributed by (i)
partition by range (d) (start (date '2001-01-01') end (date '2005-01-01')
every(interval '1 year'));
COPY foz FROM stdin DELIMITER '|';
1|2001-01-2
2|2001-10-10
3|2002-10-30
4|2003-01-01
5|2004-05-05
\.
select * from foz_1_prt_1;
select * from foz_1_prt_2;
select * from foz_1_prt_3;
select * from foz_1_prt_4;

-- Check behaviour of key for which there is no partition
COPY foz FROM stdin DELIMITER '|';
6|2010-01-01
\.
drop table foz cascade;
-- Same test with append only
create table foz (i int, d date) with (appendonly = true) distributed by (i)
partition by range (d) (start (date '2001-01-01') end (date '2005-01-01')
every(interval '1 year'));
COPY foz FROM stdin DELIMITER '|';
1|2001-01-2
2|2001-10-10
3|2002-10-30
4|2003-01-01
5|2004-05-05
\.
select * from foz_1_prt_1;
select * from foz_1_prt_2;
select * from foz_1_prt_3;
select * from foz_1_prt_4;

-- Check behaviour of key for which there is no partition
COPY foz FROM stdin DELIMITER '|';
6|2010-01-01
\.
drop table foz cascade;


-- complain if create table as select (CTAS)

CREATE TABLE rank1 (id int,
rank int, year date, gender char(1));

create table rank2 as select * from rank1
DISTRIBUTED BY (id, gender, year)
partition by list (gender)
subpartition by range (year)
subpartition template (
start (date '2001-01-01')
end (date '2006-01-01') every (interval '1 year')) (
partition boys values ('M'),
partition girls values ('F')
);

-- like is ok

create table rank2 (like rank1)
DISTRIBUTED BY (id, gender, year)
partition by list (gender)
subpartition by range (year)
subpartition template (
start (date '2001-01-01')
end (date '2006-01-01') every (interval '1 year')) (
partition boys values ('M'),
partition girls values ('F')
);

drop table rank1 cascade;
drop table rank2 cascade;


-- alter table testing

create table hhh (a char(1), b date, d char(3))
distributed by (a)
partition by range (b)
(
partition aa start (date '2007-01-01') end (date '2008-01-01') 
    with (appendonly=true),
partition bb start (date '2008-01-01') end (date '2009-01-01')
    with (appendonly=false)
);

-- already exists
alter table hhh add partition aa;

-- no partition spec
alter table hhh add partition cc;

-- overlaps
alter table hhh add partition cc start ('2008-01-01') end ('2010-01-01');
alter table hhh add partition cc end ('2008-01-01');

-- reversed (start > end)
alter table hhh add partition cc start ('2010-01-01') end ('2009-01-01');

-- works
--alter table hhh add partition cc start ('2009-01-01') end ('2010-01-01');
alter table hhh add partition cc end ('2010-01-01');

-- works - anonymous partition MPP-3350
alter table hhh add partition end ('2010-02-01');

-- MPP-3607 - ADD PARTITION with open intervals
create table no_end1 (aa int, bb int) partition by range (bb)
(partition foo start(3));

-- fail overlap
alter table no_end1 add partition baz end (4);

-- fail overlap (because prior partition has no end)
alter table no_end1 add partition baz start (5);

-- ok (terminates on foo start)
alter table no_end1 add partition baz start (2);

-- ok (because ends before baz start)
alter table no_end1 add partition baz2 end (1);

create table no_start1 (aa int, bb int) partition by range (bb)
(partition foo end(3));

-- fail overlap (because next partition has no start)
alter table no_start1 add partition baz start (2);

-- fail overlap (because next partition has no start)
alter table no_start1 add partition baz end (1);

-- ok (starts on foo end)
alter table no_start1 add partition baz end (4);

-- ok (because starts after baz end)
alter table no_start1 add partition baz2 start (5);

select tablename, partitionlevel, parentpartitiontablename,
partitionname, partitionrank, partitionboundary from pg_partitions
where tablename = 'no_start1' or tablename = 'no_end1' 
order by tablename, partitionrank;

drop table no_end1;
drop table no_start1;

-- hash partitions cannot have default partitions
create table jjj (aa int, bb int) 
partition by hash(bb) 
(partition j1, partition j2);

alter table jjj add default partition;

drop table jjj cascade;

-- default partitions cannot have boundary specifications
create table jjj (aa int, bb date) 
partition by range(bb) 
(partition j1 end (date '2008-01-01'), 
partition j2 end (date '2009-01-01'));

-- must have a name
alter table jjj add default partition;
alter table jjj add default partition for (rank(1));
-- cannot have boundary spec
alter table jjj add default partition j3 end (date '2010-01-01');

drop table jjj cascade;

-- only one default partition
create table jjj (aa int, bb date) 
partition by range(bb) 
(partition j1 end (date '2008-01-01'), 
partition j2 end (date '2009-01-01'),
default partition j3);

alter table jjj add default partition j3 ;
alter table jjj add default partition j4 ;

-- cannot add if have default, must split
alter table jjj add partition j5 end (date '2010-01-01');

drop table jjj cascade;

alter table hhh alter partition cc set tablespace foo;

alter table hhh alter partition aa set tablespace foo;

alter table hhh coalesce partition cc;

alter table hhh coalesce partition aa;

alter table hhh drop partition cc;

alter table hhh drop partition cc cascade;

alter table hhh drop partition cc restrict;

alter table hhh drop partition if exists cc;

-- fail (mpp-3265)
alter table hhh drop partition for (rank(0));
alter table hhh drop partition for (rank(-55));
alter table hhh drop partition for ('2001-01-01');


create table hhh_r1 (a char(1), b date, d char(3))
distributed by (a)
partition by range (b)
(
partition aa start (date '2007-01-01') end (date '2008-01-01') 
             every (interval '1 month')
);

create table hhh_l1 (a char(1), b date, d char(3))
distributed by (a)
partition by list (b)
(
partition aa values ('2007-01-01'),
partition bb values ('2008-01-01'),
partition cc values ('2009-01-01') 
);

-- must have name or value for list partition
alter table hhh_l1 drop partition;
alter table hhh_l1 drop partition aa;
alter table hhh_l1 drop partition for ('2008-01-01');

-- if not specified, drop first range partition...
alter table hhh_r1 drop partition for ('2007-04-01');
alter table hhh_r1 drop partition;
alter table hhh_r1 drop partition;
alter table hhh_r1 drop partition;
alter table hhh_r1 drop partition;
alter table hhh_r1 drop partition;

-- more add partition tests

-- start before first partition (fail because start equal end)
alter table hhh_r1 add partition zaa start ('2007-07-01');
-- start before first partition (ok)
alter table hhh_r1 add partition zaa start ('2007-06-01');
-- start > last (fail because start equal end)
alter table hhh_r1 add partition bb start ('2008-01-01') end ('2008-01-01') ;
-- start > last (ok)
alter table hhh_r1 add partition bb start ('2008-01-01') 
end ('2008-02-01') inclusive;
-- start > last (fail because start == last end inclusive)
alter table hhh_r1 add partition cc start ('2008-02-01') end ('2008-03-01') ;
-- start > last (ok [and leave a gap])
alter table hhh_r1 add partition cc start ('2008-04-01') end ('2008-05-01') ;
-- overlap (fail)
alter table hhh_r1 add partition dd start ('2008-01-01') end ('2008-05-01') ;
-- new partition in "gap" (ok)
alter table hhh_r1 add partition dd start ('2008-03-01') end ('2008-04-01') ;
-- overlap all partitions (fail)
alter table hhh_r1 add partition ee start ('2006-01-01') end ('2009-01-01') ;
-- start before first partition (fail because end in "gap" [and overlaps])
alter table hhh_r1 add partition yaa start ('2007-05-01') end ('2007-07-01');
-- start before first partition (fail )
alter table hhh_r1 add partition yaa start ('2007-05-01') 
end ('2007-10-01') inclusive;
-- start before first partition (fail because end overlaps)
alter table hhh_r1 add partition yaa start ('2007-05-01') 
end ('2007-10-01') exclusive;


drop table hhh_r1 cascade;
drop table hhh_l1 cascade;


--  the documentation example, rewritten with EVERY in a template
--  and also with a default partition
CREATE TABLE rank (id int,
rank int, year date, gender char(1))
DISTRIBUTED BY (id, gender, year)
partition by list (gender)
subpartition by range (year)
subpartition template (
start (date '2001-01-01')
end (date '2006-01-01') every (interval '1 year')) (
partition boys values ('M'),
partition girls values ('F'),
default partition neuter
);

insert into rank values (1, 1, date '2001-01-15', 'M');
insert into rank values (2, 1, date '2002-02-15', 'M');
insert into rank values (3, 1, date '2003-03-15', 'M');
insert into rank values (4, 1, date '2004-04-15', 'M');
insert into rank values (5, 1, date '2005-05-15', 'M');
insert into rank values (6, 1, date '2001-01-15', 'F');
insert into rank values (7, 1, date '2002-02-15', 'F');
insert into rank values (8, 1, date '2003-03-15', 'F');
insert into rank values (9, 1, date '2004-04-15', 'F');
insert into rank values (10, 1, date '2005-05-15', 'F');


select * from rank ;

alter table rank DROP partition boys restrict;

select * from rank ;

-- MPP-3722: complain if for(value) matches the default partition 
alter table rank truncate partition for('N');
alter table rank DROP partition for('N');
alter table rank DROP partition if exists for('N');

alter table rank DROP default partition if exists ;

-- can't drop the final partition - must drop the table
alter table rank DROP partition girls;

-- MPP-4011: make FOR(value) work
alter table rank alter partition for ('F') add default partition def1;
alter table rank alter partition for ('F') 
truncate partition for ('2010-10-10');
alter table rank truncate partition for ('F');

drop table rank cascade;

alter table hhh exchange partition cc with table nosuchtable with validation;

alter table hhh exchange partition cc with table nosuchtable without validation;

alter table hhh exchange partition aa with table nosuchtable with validation;

alter table hhh exchange partition aa with table nosuchtable without validation;

alter table hhh merge partition cc, partition dd;

alter table hhh merge partition cc, partition dd into partition ee;

alter table hhh merge partition aa, partition dd into partition ee;

alter table hhh modify partition cc add values ('a');
alter table hhh modify partition cc drop values ('a');
alter table hhh modify partition aa add values ('a');
alter table hhh modify partition aa drop values ('a');

create table mmm_r1 (a char(1), b date, d char(3))
distributed by (a)
partition by range (b)
(
partition aa start (date '2007-01-01') end (date '2008-01-01')
             every (interval '1 month')
);

create table mmm_l1 (a char(1), b char(1), d char(3))
distributed by (a)
partition by list (b)
(
partition aa values ('a', 'b', 'c'),
partition bb values ('d', 'e', 'f'),
partition cc values ('g', 'h', 'i')
);

alter table mmm_r1 drop partition for ('2007-03-01');
-- ok
alter table mmm_r1 add partition bb START ('2007-03-03') END ('2007-03-20');

-- fail
alter table mmm_r1 modify partition for (rank(-55)) start ('2007-03-02');
alter table mmm_r1 modify partition for ('2001-01-01') start ('2007-03-02');
alter table mmm_r1 modify partition bb start ('2006-03-02');
alter table mmm_r1 modify partition bb start ('2011-03-02');
alter table mmm_r1 modify partition bb end ('2006-03-02');
alter table mmm_r1 modify partition bb end ('2011-03-02');
alter table mmm_r1 modify partition bb add values ('2011-03-02');
alter table mmm_r1 modify partition bb drop values ('2011-03-02');

--ok
alter table mmm_r1 modify partition bb START ('2007-03-02') END ('2007-03-22');
alter table mmm_r1 modify partition bb START ('2007-03-01') END ('2007-03-31');
alter table mmm_r1 modify partition bb START ('2007-03-02') END ('2007-03-22');

-- with default
alter table mmm_r1 add default partition def1;

-- now fail
alter table mmm_r1 modify partition bb START ('2007-03-01') END ('2007-03-31');

-- still ok to reduce range
alter table mmm_r1 modify partition bb START ('2007-03-09') END ('2007-03-10');

-- fail
alter table mmm_l1 modify partition for (rank(1)) drop values ('k');
alter table mmm_l1 modify partition for ('j') drop values ('k');
alter table mmm_l1 modify partition for ('a') drop values ('k');
alter table mmm_l1 modify partition for ('a') drop values ('e');
alter table mmm_l1 modify partition for ('a') add values ('e');

alter table mmm_l1 modify partition for ('a') START ('2007-03-09') ;


--ok
alter table mmm_l1 modify partition for ('a') drop values ('b');
alter table mmm_l1 modify partition for ('a') add values ('z');

-- with default
alter table mmm_l1 add default partition def1;
-- ok
alter table mmm_l1 modify partition for ('a') drop values ('c');
-- now fail
alter table mmm_l1 modify partition for ('a') add values ('y');

-- XXX XXX: add some data 

drop table mmm_r1 cascade;
drop table mmm_l1 cascade;



alter table hhh rename partition cc to aa;
alter table hhh rename partition aa to aa;
alter table hhh rename partition aa to "funky fresh";
alter table hhh rename partition "funky fresh" to aa;

-- use FOR PARTITION VALUE (with implicate date conversion)
alter table hhh rename partition for ('2007-01-01') to "funky fresh";
alter table hhh rename partition for ('2007-01-01') to aa;


alter table hhh set subpartition template ();

alter table hhh split partition cc at ('a');
alter table hhh split partition cc at ('a') into (partition gg, partition hh);
alter table hhh split partition aa at ('a');

alter table hhh truncate partition cc;

alter table hhh truncate partition aa;

insert into hhh values('a', date '2007-01-02', 'b');
insert into hhh values('a', date '2007-02-01', 'b');
insert into hhh values('a', date '2007-03-01', 'b');
insert into hhh values('a', date '2007-04-01', 'b');
insert into hhh values('a', date '2007-05-01', 'b');
insert into hhh values('a', date '2007-06-01', 'b');
insert into hhh values('a', date '2007-07-01', 'b');
insert into hhh values('a', date '2007-08-01', 'b');
insert into hhh values('a', date '2007-09-01', 'b');
insert into hhh values('a', date '2007-10-01', 'b');
insert into hhh values('a', date '2007-11-01', 'b');
insert into hhh values('a', date '2007-12-01', 'b');
insert into hhh values('a', date '2008-01-02', 'b');
insert into hhh values('a', date '2008-02-01', 'b');
insert into hhh values('a', date '2008-03-01', 'b');
insert into hhh values('a', date '2008-04-01', 'b');
insert into hhh values('a', date '2008-05-01', 'b');
insert into hhh values('a', date '2008-06-01', 'b');
insert into hhh values('a', date '2008-07-01', 'b');
insert into hhh values('a', date '2008-08-01', 'b');
insert into hhh values('a', date '2008-09-01', 'b');
insert into hhh values('a', date '2008-10-01', 'b');
insert into hhh values('a', date '2008-11-01', 'b');
insert into hhh values('a', date '2008-12-01', 'b');

select * from hhh;

alter table hhh truncate partition aa;

select * from hhh;

alter table hhh truncate partition bb;

select * from hhh;

insert into hhh values('a', date '2007-01-02', 'b');
insert into hhh values('a', date '2007-02-01', 'b');
insert into hhh values('a', date '2007-03-01', 'b');
insert into hhh values('a', date '2007-04-01', 'b');
insert into hhh values('a', date '2007-05-01', 'b');
insert into hhh values('a', date '2007-06-01', 'b');
insert into hhh values('a', date '2007-07-01', 'b');
insert into hhh values('a', date '2007-08-01', 'b');
insert into hhh values('a', date '2007-09-01', 'b');
insert into hhh values('a', date '2007-10-01', 'b');
insert into hhh values('a', date '2007-11-01', 'b');
insert into hhh values('a', date '2007-12-01', 'b');
insert into hhh values('a', date '2008-01-02', 'b');
insert into hhh values('a', date '2008-02-01', 'b');
insert into hhh values('a', date '2008-03-01', 'b');
insert into hhh values('a', date '2008-04-01', 'b');
insert into hhh values('a', date '2008-05-01', 'b');
insert into hhh values('a', date '2008-06-01', 'b');
insert into hhh values('a', date '2008-07-01', 'b');
insert into hhh values('a', date '2008-08-01', 'b');
insert into hhh values('a', date '2008-09-01', 'b');
insert into hhh values('a', date '2008-10-01', 'b');
insert into hhh values('a', date '2008-11-01', 'b');
insert into hhh values('a', date '2008-12-01', 'b');

select * from hhh;

-- truncate child partitions recursively
truncate table hhh;

select * from hhh;


drop table hhh cascade;

-- default partitions

-- hash partitions cannot have default partitions
create table jjj (aa int, bb int) 
partition by hash(bb) 
(partition j1, partition j2, default partition j3);

-- default partitions cannot have boundary specifications
create table jjj (aa int, bb date) 
partition by range(bb) 
(partition j1 end (date '2008-01-01'), 
partition j2 end (date '2009-01-01'), 
default partition j3 end (date '2010-01-01'));

-- more than one default partition
create table jjj (aa int, bb date) 
partition by range(bb) 
(partition j1 end (date '2008-01-01'), 
partition j2 end (date '2009-01-01'), 
default partition j3,
default partition j4);


-- check default
create table foz (i int, d date) distributed by (i)
partition by range (d) 
(
 default partition dsf,
 partition foo start (date '2001-01-01') end (date '2005-01-01')
               every(interval '1 year')
);
insert into foz values(1, '2003-04-01');
insert into foz values(2, '2010-04-01');
select * from foz;
select * from foz_1_prt_dsf;
drop table foz cascade;

-- check for out of order partition definitions. We should order these correctly
-- and determine the appropriate boundaries.
create table d (i int, j int) distributed by (i) partition by range(j)
( start (10), start(5), start(50) end(60));
insert into d values(1, 5);
insert into d values(1, 10);
insert into d values(1, 11);
insert into d values(1, 55);
insert into d values(1, 70);
select * from d;
select * from d_1_prt_1;
select * from d_1_prt_2;
select * from d_1_prt_3;
drop table d cascade;

-- check for NULL support
-- hash
create table d (i int, j int) partition by hash(j) partitions 4;
insert into d values(1, NULL);
insert into d values(NULL, NULL);
drop table d cascade;
-- list
create table d (i int, j int) partition by list(j)
(partition a values(1, 2, NULL),
 partition b values(3, 4)
);
insert into d values(1, 1);
insert into d values(1, 2);
insert into d values(1, NULL);
insert into d values(1, 3);
insert into d values(1, 4);
select * from d_1_prt_a;
select * from d_1_prt_b;
drop table d cascade;
--range
-- Reject NULL values
create table d (i int,  j int) partition by range(j)
(partition a start (1) end(10), partition b start(11) end(20));
insert into d values (1, 1);
insert into d values (1, 2);
insert into d values (1, NULL);
drop table  d cascade;
-- allow NULLs into the default partition
create table d (i int,  j int) partition by range(j)
(partition a start (1) end(10), partition b start(11) end(20),
default partition abc);
insert into d values (1, 1);
insert into d values (1, 2);
insert into d values (1, NULL);
select * from d_1_prt_abc;
drop table  d cascade;

-- multicolumn list support
create table d (a int, b int, c int) distributed by (a) 
partition by list(b, c)
(partition a values(('1', '2'), ('3', '4')),
 partition b values(('100', '20')),
 partition c values(('1000', '1001'), ('1001', '1002'), ('1003', '1004')));

insert into d values(1, 1, 2);
insert into d values(1, 3, 4);
insert into d values(1, 100, 20);
insert into d values(1, 100, 2000);
insert into d values(1, '1000', '1001'), (1, '1001', '1002'), (1, '1003', '1004');
insert into d values(1, 100, NULL);
select * from d_1_prt_a;
select * from d_1_prt_b;
select * from d_1_prt_c;
drop table d cascade;

-- test multi value range partitioning
create table b (i int, j date) distributed by (i)
partition by range (i, j)
(start(1, '2008-01-01') end (10, '2009-01-01'),
 start(1, '2009-01-01') end(15, '2010-01-01'),
 start(15, '2010-01-01') end (30, '2011-01-01'),
 start(1, '2011-01-01') end (100, '2012-01-01')
);
-- should work
insert into b values(1, '2008-06-11');
insert into b values(11, '2009-08-24');
insert into b values(25, '2010-01-22');
insert into b values(90, '2011-05-04');
-- shouldn't work
insert into b values(1, '2019-01-01');
insert into b values(91, '2008-05-05');
 
select * from b_1_prt_1;
select * from b_1_prt_2;
select * from b_1_prt_3;
select * from b_1_prt_4;
drop table b;

-- try some different combinations
create table b (i int, n numeric(20, 2), t timestamp, s text)
distributed by (i)
partition by range(n, t, s)
(
start(2000.99, '2007-01-01 00:00:00', 'AAA')
  end (4000.95, '2007-02-02 15:00:00', 'BBB'),

start(2000.99, '2007-01-01 00:00:00', 'BBB')
  end (4000.95, '2007-02-02 16:00:00', 'CCC'),

start(4000.95, '2007-01-01 00:00:00', 'AAA')
  end (7000.95, '2007-02-02 15:00:00', 'BBB')
);

-- should work
insert into b values(1, 2000.99, '2007-01-01 00:00:00', 'AAA');
insert into b values(2, 2000.99, '2007-01-01 00:00:00', 'BBB');
insert into b values(3, 4000.95, '2007-01-01 00:00:00', 'AAA');
insert into b values(6, 3000, '2007-02-02 15:30:00', 'BBC');
insert into b values(6, 3000, '2007-02-02 15:30:00', 'CC');
insert into b values(6, 3000, '2007-02-02 16:00:00'::timestamp - 
					'1 second'::interval, 'BBZZZZZZZZZZ');

-- should fail
insert into b values(6, 3000, '2007-02-02 15:30:00', 'CCCCCCC');
insert into b values(4, 5000, '2007-01-01 12:00:00', 'BCC');
insert into b values(5, 8000, '2007-01-01 12:00:00', 'ZZZ');
insert into b values(6, 3000, '2007-02-02 16:00:00', 'ABZZZZZZZZZZ');
insert into b values(6, 1000, '2007-02-02 16:00:00', 'ABZZZZZZZZZZ');
insert into b values(6, 3000, '2006-02-02 16:00:00', 'ABZZZZZZZZZZ');
insert into b values(6, 3000, '2007-02-02 00:00:00', 'A');

-- NULL tests
insert into b default values;
insert into b values(6, 3000, '2007-01-01 12:00:00', NULL);
drop table b;

-- check that we detect subpartitions partitioning a column that is already
-- a partitioning target
create table a (i int, b int)
distributed by (i)
partition by range (i)
subpartition by hash(b) subpartitions 3,
subpartition by hash(b) subpartitions 2
(start(1) end(100),
 start(100) end(1000)
);
-- MPP-3988: allow same column in multiple partitioning keys at
-- different levels -- so this is legal again...
drop table if exists a;

-- Check multi level partition COPY
CREATE TABLE REGION (
                    R_REGIONKEY INTEGER,
                    R_NAME CHAR(25),
                    R_COMMENT VARCHAR(152),
                    primary key (r_regionkey)
                    )
partition by hash (r_regionkey) partitions 1
subpartition by hash (r_name) subpartitions 3
,subpartition by hash (r_comment) subpartitions 2
(
partition p1(subpartition sp1,subpartition sp2,subpartition sp3)
);
copy region from stdin with delimiter '|';
0|AFRICA|lar deposits. blithely final packages cajole. regular waters are 
1|AMERICA|hs use ironic, even requests. s
2|ASIA|ges. thinly even pinto beans ca
3|EUROPE|ly final courts cajole furiously final excuse
4|MIDDLE EAST|uickly special accounts cajole carefully blithely close 
5|AUSTRALIA|sdf
6|ANTARCTICA|dsfdfg
\.

-- Test indexes
set enable_seqscan to off;
select * from region where r_regionkey = 1;
select * from region where r_regionkey = 2;
select * from region where r_regionkey = 3;
select * from region where r_regionkey = 4;
select * from region where r_regionkey = 5;
select * from region where r_regionkey = 6;

-- Test indexes with insert
insert into region values(7, 'abc', 'def');
select * from region where r_regionkey = '7';
-- test duplicate key. We shouldn't really allow primary keys on partitioned
-- tables since we cannot enfoce them. But since this insert maps to a 
-- single definitive partition, we can detect it.
insert into region values(7, 'abc', 'def');

drop table region;

-- exchange
-- 1) test all sanity checking

-- policies are different
create table foo (i int, j int) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));
create table bar (i int, j int) distributed by (j);
-- should fail
alter table foo exchange partition for(rank(6)) with table bar;
drop table foo;
drop table bar;

-- random policy vs. hash policy
create table foo (i int, j int) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));
create table bar (i int, j int) distributed randomly;
-- should fail
alter table foo exchange partition for(rank(6)) with table bar;
drop table foo;
drop table bar;

-- different number of columns
create table foo (i int, j int, k text) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));
create table bar (i int, j int) distributed by (i);
-- should fail
alter table foo exchange partition for(rank(6)) with table bar;
drop table foo;
drop table bar;

-- different types
create table foo (i int, j int) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));
create table bar (i int, j int8) distributed by (i);
-- should fail
alter table foo exchange partition for(rank(6)) with table bar;
drop table foo;
drop table bar;

-- different column names
create table foo (i int, j int) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));
create table bar (i int, m int) distributed by (i);
-- should fail
alter table foo exchange partition for(rank(6)) with table bar;
drop table foo;
drop table bar;

-- different owner 
create role part_role;
create table foo (i int, j int) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));
create table bar (i int, j int) distributed by (i);
set session authorization part_role;
-- should fail
alter table foo exchange partition for(rank(6)) with table bar;

-- back to super user
\c -
alter table bar owner to part_role;
set session authorization part_role;
-- should fail
alter table foo exchange partition for(rank(6)) with table bar;
\c -

-- owners should be the same, error out 
alter table foo exchange partition for(rank(6)) with table bar;
drop table foo;
drop table bar;
drop role part_role;
-- with and without OIDs
create table foo (i int, j int) with (oids = true) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));
create table bar (i int, j int) with (oids = false) distributed by (i);
-- should fail
alter table foo exchange partition for(rank(6)) with table bar;
drop table foo;
drop table bar;

-- non-partition table involved in inheritance
create table foo (i int, j int) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));

create table barparent(i int, j int) distributed by (i);
create table bar () inherits(barparent);
-- should fail
alter table foo exchange partition for(rank(6)) with table bar;
drop table foo;
drop table bar;
drop table barparent;

-- non-partition table involved in inheritance
create table foo (i int, j int) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));

create table bar(i int, j int) distributed by (i);
create table barchild () inherits(bar);
-- should fail
alter table foo exchange partition for(rank(6)) with table bar;
drop table foo;
drop table barchild;
drop table bar;
-- rules on non-partition table
create table foo (i int, j int) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));

create table bar(i int, j int) distributed by (i);
create table baz(i int, j int) distributed by (i);
create rule bar_baz as on insert to bar do instead insert into baz
  values(NEW.i, NEW.j);

alter table foo exchange partition for(rank(2)) with table bar;
drop table foo, bar, baz;

-- not yet: constraints on non-partition table
-- should fail, can't have constraints which would conflict with the
-- partitioning constraints
create table foo (i int, j int) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));

create table bar(i int, j int check (j > 1000)) distributed by (i);
alter table foo exchange partition for(rank(2)) with table bar;
drop table foo, bar;

-- shouldn't fail: it's fine to have constraints on columns which are not
-- the subject of partitioning.
create table foo (i int, j int) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));

create table bar(i int check (i > 1000), j int) distributed by (i);
alter table foo exchange partition for(rank(2)) with table bar;
drop table foo, bar;

-- shouldn't fail: check constraint matches rule
create table foo (i int, j int) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));

create table bar(i int check (i > 1000), j int check (j >= 2 and j < 3 ))
distributed by (i);
insert into bar values(100000, 2);
alter table foo exchange partition for(rank(2)) with table bar;
insert into bar values(200000, 2);
select * from bar;
drop table foo, bar;

-- permissions
create role part_role;
create table foo (i int) partition by range(i)
(start(1) end(10) every(1));
create table bar (i int);
grant select on foo to part_role;
revoke all on bar from part_role;
select has_table_privilege('part_role', 'foo_1_prt_6'::regclass, 'select');
select has_table_privilege('part_role', 'bar'::regclass, 'select');
alter table foo exchange partition for(rank(6)) with table bar;
select has_table_privilege('part_role', 'foo_1_prt_6'::regclass, 'select');
select has_table_privilege('part_role', 'bar'::regclass, 'select');
drop table foo;
drop table bar;
drop role part_role;

-- validation
create table foo (i int) partition by range(i)
(start(1) end(10) every(1));
create table bar (i int);

insert into bar values(6);
insert into bar values(100);
-- should fail
alter table foo exchange partition for(rank(6)) with table bar;
alter table foo exchange partition for(rank(6)) with table bar without
validation;
select * from foo;
drop table foo, bar;

-- basic test
create table foo (i int, j int) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));
create table bar(i int, j int) distributed by (i);

insert into bar values(6);
alter table foo exchange partition for(rank(6)) with table bar;
select * from foo;
select * from bar;
-- test that we got the dependencies right
drop table bar;
select * from foo;
drop table foo;
create table foo (i int, j int) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));
create table bar(i int, j int) distributed by (i);

insert into bar values(6, 6);
alter table foo exchange partition for(rank(6)) with table bar;
insert into bar values(10, 10);
drop table foo;
select * from bar;
drop table bar;

-- AO exchange with heap
create table foo (i int, j int) with(appendonly = true) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));
create table bar(i int, j int) distributed by (i);

insert into foo values(1, 1), (2, 1), (3, 1);
insert into bar values(6, 6);
alter table foo exchange partition for(rank(6)) with table bar;
select * from foo;
drop table bar;
drop table foo;

-- other way around
create table foo (i int, j int) with(appendonly = false) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));
create table bar(i int, j int) with(appendonly = true) distributed by (i);

insert into foo values(1, 1), (2, 1), (3, 2);
insert into bar values(6, 6);
alter table foo exchange partition for(rank(6)) with table bar;
select * from foo;
drop table bar;
drop table foo;

-- exchange AO with AO
create table foo (i int, j int) with(appendonly = true) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));
create table bar(i int, j int) with(appendonly = true) distributed by (i);

insert into foo values(1, 2), (2, 3), (3, 4);
insert into bar values(6, 6);
alter table foo exchange partition for(rank(6)) with table bar;
select * from foo;
drop table bar;
drop table foo;

-- exchange same table more than once
create table foo (i int, j int) distributed by (i)
partition by range(j)
(start(1) end(10) every(1));
create table bar(i int, j int) distributed by (i);

insert into bar values(6, 6);
alter table foo exchange partition for(rank(6)) with table bar;
select * from foo;
select * from bar;

alter table foo exchange partition for(rank(6)) with table bar;
select * from foo;
select * from bar;

alter table foo exchange partition for(rank(6)) with table bar;
select * from foo;
select * from bar;
drop table foo;
drop table bar;

-- XXX: not yet: VALIDATE parameter


-- Check for overflow of circular data types like time
-- Should fail
CREATE TABLE TIME_TBL_HOUR_2 (f1 time(2)) distributed by (f1)
partition by range (f1)
(
  start (time '00:00') end (time '24:00') EVERY (INTERVAL '1 hour')
);
-- Should fail
CREATE TABLE TIME_TBL_HOUR_2 (f1 time(2)) distributed by (f1)
partition by range (f1)
(
  start (time '00:00') end (time '23:59') EVERY (INTERVAL '1 hour')
);
-- Should work
CREATE TABLE TIME_TBL_HOUR_2 (f1 time(2)) distributed by (f1)
partition by range (f1)
(
  start (time '00:00') end (time '23:00') EVERY (INTERVAL '1 hour')
);
drop table TIME_TBL_HOUR_2;
-- Check for every parameters that just don't make sense
create table hhh_r1 (a char(1), b date, d char(3)) 
distributed by (a) partition by range (b)
(                                                              
partition aa start (date '2007-01-01') end (date '2008-01-01') 
      every (interval '0 days')
);

create table foo (i int) distributed by(i)
partition by range(i)
(start (1) end (20) every(0));

-- Check for ambiguous EVERY parameters
-- should fail
create table foo (i int) distributed by (i)
partition by range(i)
(start (1) end (20) every (0.6));
-- should fail
create table foo (i int) distributed by (i)
partition by range(i)
(start (1) end (20) every (0.3));
-- should fail
create table foo (i int) distributed by (i)
partition by range(i)
(start (1) end (20) every (1.3));

-- should fail
create table foo (i int) distributed by (i)
partition by range(i)
(start (1) end (20) every (10.9));

-- should fail
create table foo (i int, j date) distributed by (i)
partition by range(j)
(start ('2007-01-01') end ('2008-01-01') every (interval '0.5 days'));

-- should fail
create table foo (i int, j date) distributed by (i)
partition by range(j)
(start ('2007-01-01') end ('2008-01-01') every (interval '0.5 days'));

-- should fail
create table foo (i int, j date) distributed by (i)
partition by range(j)
(start ('2007-01-01') end ('2008-01-01') every (interval '12 hours'));

-- should fail
create table foo (i int, j date) distributed by (i)
partition by range(j)
(start ('2007-01-01') end ('2008-01-01') every (interval '1.2 days'));

-- should work
create table foo (i int, j timestamp) distributed by (i)
partition by range(j)
(start ('2007-01-01') end ('2007-01-05') every (interval '1.2 days'));

drop table foo;

-- test inclusive/exclusive
CREATE TABLE supplier2(
                S_SUPPKEY INTEGER,
                S_NAME CHAR(25),
                S_ADDRESS VARCHAR(40),
                S_NATIONKEY INTEGER,
                S_PHONECHAR char(15),
                S_ACCTBAL decimal,
				S_COMMENT VARCHAR(100)
)
partition by range (s_nationkey)
(
partition p1 start(0) , 
partition p2 start(12) end(13), 
partition p3 end(20) inclusive, 
partition p4 start(20) exclusive , 
partition p5 start(22) end(25)
);

-- Make sure they're correctly ordered
select parname, parruleord, pg_get_expr(parrangestart, parchildrelid, false),
parrangestartincl,
pg_get_expr(parrangeend, parchildrelid, false),parrangeendincl 
from pg_partition_rule  where
paroid in (select oid from pg_partition where parrelid = 'supplier2'::regclass)
order by parruleord;

insert into supplier2 (s_suppkey, s_nationkey) select i, i 
from generate_series(1, 24) i;
select * from supplier2_1_prt_p1 order by S_NATIONKEY;
select * from supplier2_1_prt_p2 order by S_NATIONKEY;
select * from supplier2_1_prt_p3 order by S_NATIONKEY;
select * from supplier2_1_prt_p4 order by S_NATIONKEY;
select * from supplier2_1_prt_p5 order by S_NATIONKEY;
drop table supplier2;

-- mpp3238
create table foo (i int) partition by range (i)
(
 partition p1 start('1') ,
 partition p2 start('2639161') ,
 partition p3 start('5957166') ,
 partition p4 start('5981976') end('5994376') inclusive,
 partition p5 end('6000001')
);

select parname, parruleord, pg_get_expr(parrangestart, parchildrelid, false) as
 start, pg_get_expr(parrangeend, parchildrelid, false) as end,
 pg_get_expr(parlistvalues, parchildrelid, false) as list from 
 pg_partition_rule
 r, pg_partition p where r.paroid = p.oid and p.parlevel = 0 and 
 p.parrelid = 'foo'::regclass order by 1;

insert into foo values(5994400);
insert into foo values(1);
insert into foo values(6000002);
insert into foo values(5994376);
drop table foo;

create table foo (i int) 
partition by range(i)
(partition p1 start(1) end(5),
 partition p2 start(10),
 partition p3 end(10) exclusive);
select parname, parruleord, pg_get_expr(parrangestart, parchildrelid, false) as
 start, pg_get_expr(parrangeend, parchildrelid, false) as end,
  pg_get_expr(parlistvalues, parchildrelid, false) as list from
   pg_partition_rule
    r, pg_partition p where r.paroid = p.oid and p.parlevel = 0 and
	 p.parrelid = 'foo'::regclass order by 1;

drop table foo;
create table foo (i int) 
partition by range(i)
(partition p1 start(1) end(5),
 partition p2 start(10) exclusive,
 partition p3 end(10) inclusive);
select parname, parruleord, pg_get_expr(parrangestart, parchildrelid, false) as
 start, parrangestartincl,
 pg_get_expr(parrangeend, parchildrelid, false) as end,
 parrangeendincl,
  pg_get_expr(parlistvalues, parchildrelid, false) as list from
   pg_partition_rule
    r, pg_partition p where r.paroid = p.oid and p.parlevel = 0 and
	 p.parrelid = 'foo'::regclass order by 1;

insert into foo values(1), (5), (10);
drop table foo;

-- MPP-3264
-- mix AO with master HEAP and see if copy works
create table foo (i int)
partition by list(i)
(partition p1 values(1, 2, 3) with (appendonly = true),
 partition p2 values(4)
);

copy foo from stdin;
1
2
3
4
\.
select * from foo;
select * from foo_1_prt_p1;
select * from foo_1_prt_p2;
drop table foo;
-- other way around
create table foo (i int) with(appendonly = true)
partition by list(i)
(partition p1 values(1, 2, 3) with (appendonly = false),
 partition p2 values(4)
);

copy foo from stdin;
1
2
3
4
\.
select * from foo;
select * from foo_1_prt_p1;
select * from foo_1_prt_p2;
drop table foo;
-- MPP-3283
CREATE TABLE PARTSUPP (
PS_PARTKEY INTEGER,
PS_SUPPKEY INTEGER,
PS_AVAILQTY integer,
PS_SUPPLYCOST decimal,
PS_COMMENT VARCHAR(199)
)
partition by range (ps_suppkey)
subpartition by range (ps_partkey)
,subpartition by range (ps_supplycost) subpartition template (start('1')
end('1001') every(500))
(
partition p1 start('1') end('10001') every(5000)
(subpartition sp1 start('1') end('200001') every(66666)
)
);
insert into partsupp values(1,2,3325,771.64,', even theodolites. regular, final
theodolites eat after the carefully pending foxes. furiously regular deposits
sleep slyly. carefully bold realms above the ironic dependencies haggle
careful');
copy partsupp from stdin with delimiter '|';
1|2|3325|771.64|, even theodolites. regular, final theodolites eat after the
\.
drop table partsupp;
--MPP-3285
CREATE TABLE LINEITEM (
                L_ORDERKEY INT8,
                L_PARTKEY INTEGER,
                L_SUPPKEY INTEGER,
                L_LINENUMBER integer,
                L_QUANTITY decimal,
                L_EXTENDEDPRICE decimal,
                L_DISCOUNT decimal,
                L_TAX decimal,
                L_RETURNFLAG CHAR(1),
                L_LINESTATUS CHAR(1),
                L_SHIPDATE date,
                L_COMMITDATE date,
                L_RECEIPTDATE date,
                L_SHIPINSTRUCT CHAR(25),
                L_SHIPMODE CHAR(10),
                L_COMMENT VARCHAR(44)
                )
partition by range (l_commitdate)
(
partition p1 start('1992-01-31') end('1998-11-01') every(interval '20 months')

);
copy lineitem from stdin with delimiter '|';
18182|5794|3295|4|9|15298.11|0.04|0.01|N|O|1995-07-04|1995-05-30|1995-08-03|DELIVER IN PERSON|RAIL|y special platelets
\.

select parname, parruleord, pg_get_expr(parrangestart, parchildrelid, false) as
 start, parrangestartincl,
 pg_get_expr(parrangeend, parchildrelid, false) as end,
 parrangeendincl,
  pg_get_expr(parlistvalues, parchildrelid, false) as list from
   pg_partition_rule
    r, pg_partition p where r.paroid = p.oid and p.parlevel = 0 and
	 p.parrelid = 'lineitem'::regclass order by 1;

drop table lineitem;

-- Make sure ADD creates dependencies
create table i (i int) partition by range(i) (start (1) end(3) every(1));
alter table i add partition foo2 start(40) end (50);
drop table i;

create table i (i int) partition by range(i) (start (1) end(3) every(1));
alter table i add partition foo2 start(40) end (50);
alter table i drop partition foo2;
drop table i;

-- dumpability of partition info
create table i5 (i int) partition by RANGE(i) (start(1) exclusive end(10)
inclusive);
select tablename, partitiontablename,
partitionboundary from pg_partitions where
tablename = 'i5';
select pg_get_partition_def('i5'::regclass, true);
drop table i5;

CREATE TABLE PARTSUPP (
PS_PARTKEY INTEGER,
PS_SUPPKEY INTEGER,
PS_AVAILQTY integer,
PS_SUPPLYCOST decimal,
PS_COMMENT VARCHAR(199)
)
partition by range (ps_suppkey)
subpartition by range (ps_partkey)
,subpartition by range (ps_supplycost) subpartition template (start('1')
end('1001') every(500))
(
partition p1 start('1') end('10001') every(5000)
(subpartition sp1 start('1') end('200001') every(66666)
)
);

select tablename, partitiontablename,
partitionboundary from pg_partitions where
tablename = 'partsupp';
select pg_get_partition_def('partsupp'::regclass, true);
drop table partsupp;
set gp_enable_hash_partitioned_tables = true;
create table i5 (i int, g text) partition by list(g) 
  subpartition by hash(i) subpartitions 3
(partition p1 values('foo', 'bar'), partition p2 values('foz')
);
select tablename, partitiontablename,
partitionboundary from pg_partitions where
tablename = 'i5';
select pg_get_partition_def('i5'::regclass, true);
drop table i5;
set gp_enable_hash_partitioned_tables = false;

-- ALTER TABLE ALTER PARTITION tests

CREATE TABLE rank2 (id int, rank int,
year date, gender char(1),
usstate char(2))
DISTRIBUTED BY (id, gender, year, usstate)
partition by list (gender)
subpartition by range (year)
subpartition template (
subpartition jan01 start (date '2001-01-01'),
subpartition jan02 start (date '2002-01-01'),
subpartition jan03 start (date '2003-01-01'),
subpartition jan04 start (date '2004-01-01'),
subpartition jan05 start (date '2005-01-01')
),
subpartition by list (usstate)
subpartition template (
subpartition mass values ('MA'),
subpartition cali values ('CA'),
subpartition ohio values ('OH')
)
(
  partition boys values ('M'),
  partition girls values ('F')
);

-- and without subpartition templates...
CREATE TABLE rank3 (id int, rank int,
year date, gender char(1),
usstate char(2))
DISTRIBUTED BY (id, gender, year, usstate)
partition by list (gender)
subpartition by range (year),
subpartition by list (usstate)
(
  partition boys values ('M') 
(
subpartition jan01 start (date '2001-01-01') 
(
subpartition mass values ('MA'),
subpartition cali values ('CA'),
subpartition ohio values ('OH')
),
subpartition jan02 start (date '2002-01-01')
(
subpartition mass values ('MA'),
subpartition cali values ('CA'),
subpartition ohio values ('OH')
),
subpartition jan03 start (date '2003-01-01')
(
subpartition mass values ('MA'),
subpartition cali values ('CA'),
subpartition ohio values ('OH')
),
subpartition jan04 start (date '2004-01-01')
(
subpartition mass values ('MA'),
subpartition cali values ('CA'),
subpartition ohio values ('OH')
),
subpartition jan05 start (date '2005-01-01')
(
subpartition mass values ('MA'),
subpartition cali values ('CA'),
subpartition ohio values ('OH')
)
)
,
  partition girls values ('F')
(
subpartition jan01 start (date '2001-01-01')
(
subpartition mass values ('MA'),
subpartition cali values ('CA'),
subpartition ohio values ('OH')
),
subpartition jan02 start (date '2002-01-01')
(
subpartition mass values ('MA'),
subpartition cali values ('CA'),
subpartition ohio values ('OH')
),
subpartition jan03 start (date '2003-01-01')
(
subpartition mass values ('MA'),
subpartition cali values ('CA'),
subpartition ohio values ('OH')
),
subpartition jan04 start (date '2004-01-01')
(
subpartition mass values ('MA'),
subpartition cali values ('CA'),
subpartition ohio values ('OH')
),
subpartition jan05 start (date '2005-01-01')
(
subpartition mass values ('MA'),
subpartition cali values ('CA'),
subpartition ohio values ('OH')
)
)
);

-- ok
alter table rank2 truncate partition girls;
alter table rank2 alter partition girls truncate partition for (rank(1));
alter table rank2 alter partition girls alter partition 
for (rank(1)) truncate partition mass;

-- don't NOTIFY of children if cascade
alter table rank2 truncate partition girls cascade;

-- fail - no rank 100
alter table rank2 alter partition girls truncate partition for (rank(100));

-- fail - no funky
alter table rank2 alter partition girls alter partition 
for (rank(1)) truncate partition "funky";

-- fail - no funky (drop)
alter table rank2 alter partition girls alter partition 
for (rank(1)) drop partition "funky";

-- fail - missing name
alter table rank2 alter partition girls alter partition 
for (rank(1)) drop partition ;

-- ok
alter table rank2 alter partition girls drop partition 
for (rank(1)) ;

-- ok , skipping
alter table rank2 alter partition girls drop partition if exists jan01;

-- ok until run out of partitions
alter table rank2 alter partition girls drop partition ;
alter table rank2 alter partition girls drop partition ;
alter table rank2 alter partition girls drop partition ;
alter table rank2 alter partition girls drop partition ;
alter table rank2 alter partition girls drop partition ;

-- ok, skipping
alter table rank2 alter partition girls drop partition if exists for (rank(5));

-- ok
alter table rank2 alter partition girls rename partition jan05 
to "funky fresh";
alter table rank2 alter partition girls rename partition "funky fresh"
to jan05;

-- fail , not exist
alter table rank2 alter partition girls alter partition jan05 rename
partition jan01 to foo;

-- fail not exist
alter table rank2 alter partition girls alter partition jan05 alter
partition cali rename partition foo to bar;

-- fail not partitioned
alter table rank2 alter partition girls alter partition jan05 alter
partition cali alter partition foo drop partition bar;

-- ADD PARTITION, with and without templates

-- fails for rank2 (due to template), works for rank3
alter table rank2
add partition neuter values ('N')
    (subpartition foo
         start ('2001-01-01') end ('2002-01-01')
         every (interval '1 month')
            (subpartition bar values ('AZ')));
alter table rank3
add partition neuter values ('N')
    (subpartition foo
         start ('2001-01-01') end ('2002-01-01')
         every (interval '1 month')
            (subpartition bar values ('AZ')));

-- fail , no subpartition spec for rank3, works for rank2
alter table rank2 alter partition boys
add partition jan00 start ('2000-01-01') end ('2001-01-01');
alter table rank3 alter partition boys
add partition jan00 start ('2000-01-01') end ('2001-01-01');

-- work - create subpartition for rank3, fail for rank2
alter table rank2 alter partition boys
add partition jan99 start ('1999-01-01') end ('2000-01-01')
  (subpartition ariz values ('AZ'));
alter table rank3 alter partition boys
add partition jan00 start ('2000-01-01') end ('2001-01-01')
  (subpartition ariz values ('AZ'));

-- works for both -- adding leaf partition doesn't conflict with template
alter table rank2 alter partition boys
alter partition jan00 
add partition haw values ('HI');
alter table rank3 alter partition boys
alter partition jan00 
add partition haw values ('HI');

alter table rank2 drop partition neuter;
alter table rank3 drop partition neuter;

-- fail , no subpartition spec for rank3, work for rank2
alter table rank2
add default partition neuter ;
alter table rank3
add default partition neuter ;

alter table rank2
add default partition neuter 
    (subpartition foo
         start ('2001-01-01') end ('2002-01-01')
         every (interval '1 month')
            (subpartition ariz values ('AZ')));
alter table rank3
add default partition neuter 
    (subpartition foo
         start ('2001-01-01') end ('2002-01-01')
         every (interval '1 month')
            (subpartition ariz values ('AZ')));

-- fail
alter table rank2
alter default partition add default partition def1 
(subpartition haw values ('HI'));
-- fail
alter table rank2
alter default partition alter default partition 
add default partition def2;
-- work
alter table rank2
alter default partition add default partition def1;
alter table rank2
alter default partition alter default partition 
add default partition def2;



alter table rank3
alter default partition add default partition def1 
(subpartition haw values ('HI'));
alter table rank3
alter default partition alter default partition 
add default partition def2;


drop table rank2 ;
drop table rank3 ;

-- **END** ALTER TABLE ALTER PARTITION tests

-- Test casting
create table f (i int) partition by range (i) (start(1::int) end(10::int));
drop table f;
create table f (i bigint) partition by range (i) (start(1::int8)
end(1152921504606846976::int8) every(576460752303423488));
drop table f;
create table f (n numeric(20, 2)) partition by range(n) (start(1::bigint)
end(10000::bigint));
drop table f;
create table f (n numeric(20, 2)) partition by range(n) (start(1::bigint)
end(10000::text));
drop table f;
--should fail. bool -> numeric makes no sense
create table f (n numeric(20, 2)) partition by range(n) (start(1::bigint)
end('f'::bool));

-- see that grant and revoke cascade to children
create role part_role;
create table granttest (i int, j int) partition by range(i) 
subpartition by list(j) subpartition template (values(1, 2, 3))
(start(1) end(4) every(1));

select has_table_privilege('part_role', 'granttest'::regclass,'select');
select has_table_privilege('part_role', 'granttest_1_prt_1'::regclass,'select');
select has_table_privilege('part_role', 'granttest_1_prt_2'::regclass,'select');
select has_table_privilege('part_role', 'granttest_1_prt_3'::regclass,'select');
select has_table_privilege('part_role', 'granttest_1_prt_1_2_prt_1'::regclass,'select');
select has_table_privilege('part_role', 'granttest_1_prt_2_2_prt_1'::regclass,'select');
select has_table_privilege('part_role', 'granttest_1_prt_3_2_prt_1'::regclass,'select');
grant select on granttest to part_role;
select has_table_privilege('part_role', 'granttest'::regclass,'select');
select has_table_privilege('part_role', 'granttest_1_prt_1'::regclass,'select');
select has_table_privilege('part_role', 'granttest_1_prt_2'::regclass,'select');
select has_table_privilege('part_role', 'granttest_1_prt_3'::regclass,'select');
select has_table_privilege('part_role', 'granttest_1_prt_1_2_prt_1'::regclass,'select');
select has_table_privilege('part_role', 'granttest_1_prt_2_2_prt_1'::regclass,'select');
select has_table_privilege('part_role', 'granttest_1_prt_3_2_prt_1'::regclass,'select');
grant insert on granttest to part_role;
select has_table_privilege('part_role', 'granttest'::regclass,'insert');
select has_table_privilege('part_role', 'granttest_1_prt_1'::regclass,'insert');
select has_table_privilege('part_role', 'granttest_1_prt_2'::regclass,'insert');
select has_table_privilege('part_role', 'granttest_1_prt_3'::regclass,'insert');
select has_table_privilege('part_role', 'granttest_1_prt_1_2_prt_1'::regclass,'insert');
select has_table_privilege('part_role', 'granttest_1_prt_2_2_prt_1'::regclass,'insert');
select has_table_privilege('part_role', 'granttest_1_prt_3_2_prt_1'::regclass,'insert');

revoke all on granttest from part_role;
select has_table_privilege('part_role', 'granttest'::regclass,'insert');
select has_table_privilege('part_role', 'granttest_1_prt_1'::regclass,'insert');
select has_table_privilege('part_role', 'granttest_1_prt_2'::regclass,'insert');
select has_table_privilege('part_role', 'granttest_1_prt_3'::regclass,'insert');
select has_table_privilege('part_role', 'granttest_1_prt_1_2_prt_1'::regclass,'insert');
select has_table_privilege('part_role', 'granttest_1_prt_2_2_prt_1'::regclass,'insert');
select has_table_privilege('part_role', 'granttest_1_prt_3_2_prt_1'::regclass,'insert');

drop table granttest;
drop role part_role;

-- deep inline + optional subpartition comma:
CREATE TABLE partsupp (
    ps_partkey integer,
    ps_suppkey integer,
    ps_availqty integer,
    ps_supplycost numeric,
    ps_comment character varying(199)
) distributed by (ps_partkey) PARTITION BY RANGE(ps_suppkey)
          SUBPARTITION BY RANGE(ps_partkey)
                  SUBPARTITION BY RANGE(ps_supplycost) 
          (
          PARTITION p1_1 START (1) END (1666667) EVERY (1666666) 
                  (
                  START (1) END (19304783) 
                          (
                          START (1::numeric) END (501::numeric) EVERY (500), 
                          START (501::numeric) END (1001::numeric) EVERY (500)
                          ), 
                  START (19304783) END (100000001) 
                          (
                          START (1::numeric) END (501::numeric) EVERY (500), 
                          START (501::numeric) END (1001::numeric) EVERY (500)
                          )
                  ), 
          PARTITION p1_2 START (1666667) END (3333333) EVERY (1666666) 
                  (
                  START (1) END (19304783) 
                          (
                          START (1::numeric) END (501::numeric) EVERY (500), 
                          START (501::numeric) END (1001::numeric) EVERY (500)
                          ), 
                  START (19304783) END (100000001) 
                          (
                          START (1::numeric) END (501::numeric) EVERY (500), 
                          START (501::numeric) END (1001::numeric) EVERY (500)
                          )
                  ), 
          PARTITION p1_3 START (3333333) END (4999999) EVERY (1666666) 
                  (
                  START (1) END (19304783) 
                          (
                          START (1::numeric) END (501::numeric) EVERY (500), 
                          START (501::numeric) END (1001::numeric) EVERY (500)
                          ), 
                  START (19304783) END (100000001) 
                          (
                          START (1::numeric) END (501::numeric) EVERY (500), 
                          START (501::numeric) END (1001::numeric) EVERY (500)
                          )
                  ), 
          PARTITION p1_4 START (4999999) END (5000001) EVERY (1666666) 
                  (
                  START (1) END (19304783) 
                          (
                          START (1::numeric) END (501::numeric) EVERY (500), 
                          START (501::numeric) END (1001::numeric) EVERY (500)
                          ), 
                  START (19304783) END (100000001) 
                          (
                          START (1::numeric) END (501::numeric) EVERY (500), 
                          START (501::numeric) END (1001::numeric) EVERY (500)
                          )
                  )
          );
drop table partsupp;

-- Accept negative values trivially:
create table g (i int) partition by range(i) (start((-1)) end(10));
drop table g;
create table g (i int) partition by range(i) (start(-1) end(10));
drop table g;

CREATE TABLE orders (
    o_orderkey bigint,
    o_custkey integer,
    o_orderstatus character(1),
    o_totalprice numeric,
    o_orderdate date,
    o_orderpriority character(15),
    o_clerk character(15),
    o_shippriority integer,
    o_comment character varying(79)
)
WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) PARTITION BY RANGE(o_orderdate)
          SUBPARTITION BY RANGE(o_custkey)
                  SUBPARTITION BY RANGE(o_orderkey) 
          (
          PARTITION p1_1 START ('1992-01-01'::date) END ('1993-06-01'::date) EVERY ('1 year 5 mons'::interval) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) 
                  (
                  SUBPARTITION sp1 START (1) END (46570) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) 
                          (
                          START (1::bigint) END (1500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (1500001::bigint) END (3000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (3000001::bigint) END (4500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (4500001::bigint) END (6000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9)
                          ), 
                  SUBPARTITION sp2 START (46570) END (150001) INCLUSIVE WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) 
                          (
                          START (1::bigint) END (1500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (1500001::bigint) END (3000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (3000001::bigint) END (4500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (4500001::bigint) END (6000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9)
                          )
                  ), 
          PARTITION p1_2 START ('1993-06-01'::date) END ('1994-11-01'::date) EVERY ('1 year 5 mons'::interval) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) 
                  (
                  SUBPARTITION sp1 START (1) END (46570) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) 
                          (
                          START (1::bigint) END (1500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (1500001::bigint) END (3000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (3000001::bigint) END (4500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (4500001::bigint) END (6000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9)
                          ), 
                  SUBPARTITION sp2 START (46570) END (150001) INCLUSIVE WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) 
                          (
                          START (1::bigint) END (1500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (1500001::bigint) END (3000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (3000001::bigint) END (4500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (4500001::bigint) END (6000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9)
                          )
                  ), 
          PARTITION p1_3 START ('1994-11-01'::date) END ('1996-04-01'::date) EVERY ('1 year 5 mons'::interval) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) 
                  (
                  SUBPARTITION sp1 START (1) END (46570) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) 
                          (
                          START (1::bigint) END (1500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (1500001::bigint) END (3000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (3000001::bigint) END (4500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (4500001::bigint) END (6000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9)
                          ), 
                  SUBPARTITION sp2 START (46570) END (150001) INCLUSIVE WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) 
                          (
                          START (1::bigint) END (1500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (1500001::bigint) END (3000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (3000001::bigint) END (4500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (4500001::bigint) END (6000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9)
                          )
                  ), 
          PARTITION p1_4 START ('1996-04-01'::date) END ('1997-09-01'::date) EVERY ('1 year 5 mons'::interval) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) 
                  (
                  SUBPARTITION sp1 START (1) END (46570) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) 
                          (
                          START (1::bigint) END (1500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (1500001::bigint) END (3000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (3000001::bigint) END (4500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (4500001::bigint) END (6000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9)
                          ), 
                  SUBPARTITION sp2 START (46570) END (150001) INCLUSIVE WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) 
                          (
                          START (1::bigint) END (1500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (1500001::bigint) END (3000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (3000001::bigint) END (4500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (4500001::bigint) END (6000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9)
                          )
                  ), 
          PARTITION p1_5 START ('1997-09-01'::date) END ('1998-08-03'::date) EVERY ('1 year 5 mons'::interval) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) 
                  (
                  SUBPARTITION sp1 START (1) END (46570) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) 
                          (
                          START (1::bigint) END (1500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (1500001::bigint) END (3000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (3000001::bigint) END (4500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (4500001::bigint) END (6000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9)
                          ), 
                  SUBPARTITION sp2 START (46570) END (150001) INCLUSIVE WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9) 
                          (
                          START (1::bigint) END (1500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (1500001::bigint) END (3000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (3000001::bigint) END (4500001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9), 
                          START (4500001::bigint) END (6000001::bigint) EVERY (1500000) WITH (appendonly=true, checksum=true, blocksize=368640, compresslevel=9)
                          )
                  )
          );
drop table orders;

-- grammar bug: MPP-3361
create table i2 (i int) partition by range(i) (start(-2::int) end(20));
drop table i2;
create table i2 (i int) partition by range(i) (start((-2)::int) end(20));
drop table i2;
create table i2 (i int) partition by range(i) (start(cast ((-2)::bigint as int))
end(20));
drop table i2;
CREATE TABLE partsupp (
    ps_partkey integer,
    ps_suppkey integer,
    ps_availqty integer,
    ps_supplycost numeric,
    ps_comment character varying(199)
) PARTITION BY RANGE(ps_supplycost)
          (
          PARTITION newpart START ((-10000)::numeric) EXCLUSIVE END (1::numeric)
,
          PARTITION p1 START (1::numeric) END (1001::numeric)
          );
drop table partsupp;

-- MPP-3379
drop table if exists tmp_nation;
CREATE TABLE tmp_nation (N_NATIONKEY INTEGER, N_NAME CHAR(25), N_REGIONKEY INTEGER, N_COMMENT VARCHAR(152))  
partition by range (n_nationkey) 
 (
partition p1 start('0')  WITH (appendonly=true,checksum=true,blocksize=1998848,compresslevel=4),  
partition p2 start('11') end('15') inclusive WITH (checksum=false,appendonly=true,blocksize=655360,compresslevel=4),
partition p3 start('15') exclusive end('19'), partition p4 start('19')  WITH (compresslevel=8,appendonly=true,checksum=false,blocksize=884736), 
partition p5 start('20')
);
delete from tmp_nation;
drop table tmp_nation;

-- SPLIT tests
-- basic sanity tests. All should pass.
create table k (i int) partition by range(i) (start(1) end(10) every(2), 
default partition mydef);
insert into k select i from generate_series(1, 100) i;
alter table k split partition mydef at (20) into (partition mydef, 
partition foo);
drop table k;

create table j (i int) partition by list(i) (partition a values(1, 2, 3, 4),
partition b values(5, 6, 7, 8));
insert into j select i from generate_series(1, 8) i;
alter table j split partition for(1) at (2, 3) into (partition fa, partition
fb);
select * from j_1_prt_fa;
select * from j_1_prt_fb;
alter table j split partition for(5) at (6);
select * from j;
-- should fail
alter table j split partition for (1) at (100);
drop table j;
create table k (i int) partition by range(i) (start(1) end(10) every(2), 
default partition mydef);
-- should fail
alter table k split default partition start(30) end (300) into (partition mydef, partition mydef);
alter table k split partition for(3) at (20);
drop table k;
-- should work
create table k (i int) partition by range(i) (start(1) end(10) every(2), 
default partition mydef);
insert into k select i from generate_series(1, 30) i;
alter table k split default partition start(15) end(20) into
(partition mydef, partition foo);
select * from k_1_prt_foo;
alter table k split default partition start(22) exclusive end(25) inclusive
into (partition bar, partition mydef);
select * from k_1_prt_bar;
alter table k split partition bar at (23) into (partition baz, partition foz);
select partitiontablename,partitionposition,partitionrangestart,
       partitionrangeend from pg_partitions where tablename = 'k'
	   order by partitionposition;
drop table k;
-- Test errors for default handling
create table k (i int) partition by range(i) (start(1) end(2), 
default partition mydef);
alter table k split partition mydef at (25) into (partition foo, partition
mydef);
drop table k;
create table k (i int) partition by list(i) (values(1), values(2),
default partition mydef);
alter table k split default partition start(10) end(20);
drop table k;

-- Check that we support int2
CREATE TABLE myINT2_TBL(q1 int2)
 partition by range (q1)
 (start (1) end (3) every (1));
insert into myint2_tbl values(1), (2);
drop table myint2_tbl;

-- check that we don't allow updates of tuples such that they would move
-- between partitions
create table v (i int, j int) partition by range(j) (start(1) end(5)
 every(2));
insert into v values(1, 1) ;
-- should work
update v set j = 2;
-- should fail
update v set j = 3;
drop table v;

-- test ao seg totals
create  or replace function aototal(relname text) returns float8 as $$
declare
  aosegname text;
  tupcount float8 := 0;
  rc int := 0;
begin

  execute 'select relname from pg_class where oid=(select relaosegrelid from pg_class where relname=''' || relname || ''')' into aosegname;
  if aosegname > 0 then
	  execute 'select tupcount from pg_aoseg.' || aosegname into tupcount;
  end if;
  return tupcount;
end; $$ language plpgsql volatile;

create table ao (i int) with (appendonly = true)
 partition by range(i)
 (start(1) end(10) every(1));

insert into ao values(1), (2), (3);

select partitiontablename, aototal(partitiontablename)
from pg_partitions where tablename = 'ao';
truncate ao;
select partitiontablename, aototal(partitiontablename)
from pg_partitions where tablename = 'ao';
copy ao from stdin;
4
5
6
\.
select partitiontablename, aototal(partitiontablename)
from pg_partitions where tablename = 'ao';
-- try SREH
copy ao from stdin log errors into ao_err segment reject limit 100;
6
7
10000
f
\.
select partitiontablename, aototal(partitiontablename)
from pg_partitions where tablename = 'ao';
drop table ao;

-- MPP-3591: make sure we get inclusive/exclusive right with every().
create table k (i int) partition by range(i)
(start(0) exclusive end(100) inclusive every(25));
select partitiontablename, partitionboundary from pg_partitions
where tablename = 'k' order by 1;
insert into k select i from generate_series(1, 100) i;
drop table k;

-- ADD and SPLIT must get inherit permissions of the partition they're
-- modifying
create role part_role;
create table a (a int, b int, c int) partition by range(a) subpartition by
range(b) subpartition template (subpartition h start(1) end(10)) 
subpartition by range(c)
subpartition template(subpartition i start(1) end(10)) 
(partition g start(1) end(2));
revoke all on a from public;
grant insert on a to part_role;
-- revoke it from one existing partition, to make sure we don't screw up
-- existing permissions
revoke all on a_1_prt_g_2_prt_h_3_prt_i from part_role;
alter table a add partition b start(40) end(50);
set session authorization part_role;
select has_table_privilege('part_role', 'a'::regclass,'insert');
select has_table_privilege('part_role', 'a_1_prt_b_2_prt_h'::regclass,'insert');
select has_table_privilege('part_role', 'a_1_prt_b_2_prt_h_3_prt_i'::regclass,'insert');
select has_table_privilege('part_role', 'a_1_prt_g_2_prt_h_3_prt_i'::regclass,
'insert');
insert into a values(45, 5, 5);
-- didn't grant select
select has_table_privilege('part_role', 'a'::regclass,'select');
select has_table_privilege('part_role', 'a_1_prt_b_2_prt_h'::regclass,'select');
select has_table_privilege('part_role', 'a_1_prt_b_2_prt_h_3_prt_i'::regclass,'select');
\c -
drop table a;
create table a (i date) partition by range(i) 
(partition f start(date '2005-01-01') end (date '2009-01-01')
	every(interval '2 years'));
revoke all on a from public;
grant insert on a to part_role;
alter table a split partition for(rank(1)) at (date '2006-01-01')
  into (partition f, partition g);
alter table a add default partition mydef;
alter table a split default partition start(date '2010-01-01') end(date
'2011-01-01') into(partition mydef, partition other);
set session authorization part_role;
select has_table_privilege('part_role', 'a'::regclass,'insert');
select has_table_privilege('part_role', 'a_1_prt_f'::regclass,'insert');
select has_table_privilege('part_role', 'a_1_prt_mydef'::regclass,'insert');
select has_table_privilege('part_role', 'a_1_prt_other'::regclass,'insert');
insert into a values('2005-05-05');
insert into a values('2006-05-05');
insert into a values('2010-10-10');
\c -
drop table a;
drop role part_role;
-- Check that when we split a default, the INTO clause must named the default
create table k (i date) partition by range(i) (start('2008-01-01')
end('2009-01-01') every(interval '1 month'), default partition default_part);
alter table k split default partition start ('2009-01-01') end ('2009-02-01')
into (partition aa, partition nodate);
alter table k split default partition start ('2009-01-01') end ('2009-02-01')
into (partition aa, partition default_part);
-- check that it works without INTO
alter table k split default partition start ('2009-02-01') end ('2009-03-01');
drop table k;
-- List too
create table k (i int) partition by list(i) (partition a values(1, 2),
partition b values(3, 4), default partition mydef);
alter table k split partition mydef at (5) into (partition foo, partition bar);
alter table k split partition mydef at (5) into (partition foo, partition mydef);
alter table k split partition mydef at (10);
drop table k;

-- For LIST, make sure that we reject AT() clauses which match all parameters
create table j (i int) partition by list(i) (partition a values(1, 2, 3, 4),
 partition b values(5, 6, 7, 8));
alter table j split partition for(1) at (1,2) into (partition fa, partition fb);
alter table j split partition for(1) at (1,2) 
into (partition f1a, partition f1b); -- This has partition rules that overlaps
drop table j;

-- Check that we can split LIST partitions that have a default partition
create table j (i int) partition by list(i) (partition a values(1, 2, 3, 4),
partition b values(5, 6, 7, 8), default partition default_part);
alter table j split partition for(1) at (1,2) into (partition f1a, partition
f1b);
drop table j;
-- Make sure range can too
create table j (i int) partition by range(i) (partition a start(1) end(10),
default partition default_part);
alter table j split partition for(1) at (5) into (partition f1a, partition f1b);
drop table j;

-- MPP-3667 ADD PARTITION overlaps
create table mpp3621 (aa date, bb date) partition by range (bb)
(partition foo start('2008-01-01'));

-- these are ok
alter table mpp3621 add partition a1 start ('2007-01-01') end ('2007-02-01');
alter table mpp3621 add partition a2 start ('2007-02-01') end ('2007-03-01');
alter table mpp3621 add partition a3 start ('2007-03-01') end ('2007-04-01');
alter table mpp3621 add partition a4 start ('2007-09-01') end ('2007-10-01');
alter table mpp3621 add partition a5 start ('2007-08-01') end ('2007-09-01');
alter table mpp3621 add partition a6 start ('2007-04-01') end ('2007-05-01');
alter table mpp3621 add partition a7 start ('2007-05-01') end ('2007-06-01');

 -- was error due to startSearchpoint != endSearchpoint
alter table mpp3621 add partition a8 start ('2007-07-01') end ('2007-08-01');

-- ok
alter table mpp3621 add partition a9 start ('2007-06-01') end ('2007-07-01');

drop table mpp3621;

-- Check for MPP-3679
create table list_test (a text, b text) partition by list (a) (partition foo
values ('foo'), partition bar values ('bar'), default partition baz); 
alter table list_test split default partition at ('baz') into (partition bing,
default partition);
drop table list_test;

-- MPP-3816: cannot drop column  which is the subject of partition config
create table list_test(a int, b int, c int) distributed by (a)
  partition by list(b) 
  subpartition by list(c) subpartition template(subpartition c values(2))
  (partition b values(1));
-- should fail
alter table list_test drop column b;
alter table list_test drop column c;
drop table list_test;

-- MPP-3678: allow exchange and split on tables with subpartitioning
CREATE TABLE rank (
id int,
rank int,
year int,
gender char(1),
count int ) 

DISTRIBUTED BY (id)
PARTITION BY LIST (gender)
SUBPARTITION BY RANGE (year)
SUBPARTITION TEMPLATE (
SUBPARTITION year1 START (2001),
SUBPARTITION year2 START (2002),
SUBPARTITION year3 START (2003),
SUBPARTITION year4 START (2004),
SUBPARTITION year5 START (2005),
SUBPARTITION year6 START (2006) END (2007) )
(PARTITION girls VALUES ('F'),
PARTITION boys VALUES ('M')
);
alter table rank alter partition girls add default partition gfuture;
alter table rank alter partition boys add default partition bfuture;
insert into rank values(1, 1, 2007, 'M', 1);
insert into rank values(2, 2, 2008, 'M', 3);
select * from rank;
alter table rank alter partition boys split default partition start ('2007')
end ('2008') into (partition bfuture, partition year7);
select * from rank_1_prt_boys_2_prt_bfuture;
select * from rank_1_prt_boys_2_prt_year7;
select * from rank;

--exchange test
create table r (like rank);
insert into rank values(3, 3, 2004, 'F', 100);
insert into r values(3, 3, 2004, 'F', 100000);
alter table rank alter partition girls exchange partition year4 with table r;
select * from rank_1_prt_girls_2_prt_year4;
select * from r;
alter table rank alter partition girls exchange partition year4 with table r;
select * from rank_1_prt_girls_2_prt_year4;
select * from r;

-- Split test
alter table rank alter partition girls split default partition start('2008')
  end('2020') into (partition years, partition gfuture);
insert into rank values(4, 4, 2009, 'F', 100);
drop table rank;
drop table r;

-- MPP-4245: remove virtual subpartition templates when we drop the partitioned
-- table
create table bar (i int, j int) partition by range(i) subpartition by range(j)
subpartition template(start(1) end(10) every(1)) subpartition by range(i)
subpartition template(start(1) end(10) every(5)) (start(1) end(10));
alter table bar alter partition for ('5') alter partition for ('5')
  drop partition for ('5');
insert into bar values(1, 1);
insert into bar values(5, 5);
drop table bar;
select parrelid::regclass, * from pg_partition;
select * from pg_partition_rule;

-- MPP-4172
-- should fail
create table ggg (a char(1), b int)
distributed by (b)
partition by range(a)
(
partition aa start ('2006') end ('2009'), partition bb start ('2007') end
('2008')
);


-- MPP-4892 SET SUBPARTITION TEMPLATE
create table mpp4892 (a char, b int, d char)
partition by range (b)
subpartition by list (d)
subpartition template (
 subpartition sp1 values ('a'),
 subpartition sp2 values ('b'))
(
start (1) end (10) every (1)
);

-- works
alter table mpp4892 add partition p1 end (11);

-- complain about existing template
alter table mpp4892 add partition p3 end (13) (subpartition sp3 values ('c'));

-- remove template
alter table mpp4892 set	subpartition template ();

-- should work (because the template is gone)
alter table mpp4892 add partition p3 end (13) (subpartition sp3 values ('c'));

-- complain because the template is already gone
alter table mpp4892 set	subpartition template ();

-- should work
alter table mpp4892 set subpartition template (subpartition sp3 values ('c'));

-- should work
alter table mpp4892 add partition p4 end (15);

drop table mpp4892;


-- make sure we do not allow overlapping range intervals
-- should fail
-- unordered elems
create table ttt (t int) partition by range(t) (
partition a start (1) end(10) inclusive,
partition c start(11) end(14),
partition b start(5) end(15)
);

-- should fail, this time it's ordered
create table ttt (t int) partition by range(t) (
partition a start (1) end(10) inclusive,
partition b start(5) end(15),
partition c start(11) end(14)
);

-- should fail
create table ttt (t date) partition by range(t) (
partition a start ('2005-01-01') end('2006-01-01') inclusive,
partition b start('2005-05-01') end('2005-06-11'),
partition c start('2006-01-01') exclusive end('2006-01-10')
);

-- should fail
create table ttt (t char) partition by range(t) (
partition a start('a') end('f'),
partition b start('e') end('g')
);

-- Test locking behaviour. When creating, dropping, querying or adding indexes
-- partitioned tables, we want to lock only the master, not the children.

create view locktest as
select coalesce(
  case when relname like 'pg_toast%index' then 'toast index'
  	   when relname like 'pg_toast%' then 'toast table'
	   else relname end, 'dropped table'), 
mode from 
pg_locks l left outer join pg_class c on (l.relation = c.oid),
pg_database d where relation is not null and l.database = d.oid and 
d.datname = current_database() order by 1, 2;
-- Partitioned table with toast table
begin;

-- creation
create table g (i int, t text) partition by range(i)
(start(1) end(10) every(1));
select * from locktest;
commit;

-- drop
begin;
drop table g;
select * from locktest;
commit;

-- AO table (ao segments)
begin;
-- creation
create table g (i int, t text, n numeric)
with (appendonly = true)
partition by list(i)
(values(1), values(2), values(3));
select * from locktest;
commit;
begin;

-- add a little data
insert into g values(1), (2), (3);
insert into g values(1), (2), (3);
insert into g values(1), (2), (3);
insert into g values(1), (2), (3);
insert into g values(1), (2), (3);

select * from locktest;

commit;
-- drop
begin;
drop table g;
select * from locktest;
commit;

-- Indexing
create table g (i int, t text) partition by range(i)
(start(1) end(10) every(1));

begin;
create index g_idx on g(i);
select * from locktest;
commit;

-- test select locking
begin;
select * from g where i = 1;
select * from locktest;
commit;

begin;
-- insert locking
insert into g values(3, 'f');
select * from locktest;
commit;

-- delete locking
begin;
delete from g where i = 4;
select * from locktest;
commit;

-- drop index
begin;
drop table g;
select * from locktest;
commit;

-- MPP-5159
-- Should fail -- missing partition spec and subpartition template follows the
-- partition declaration.
CREATE TABLE list_sales (trans_id int, date date, amount
decimal(9,2), region text)
DISTRIBUTED BY (trans_id)
PARTITION BY LIST (region)
SUBPARTITION TEMPLATE
( SUBPARTITION usa VALUES ('usa'),
  SUBPARTITION asia VALUES ('asia'),
  SUBPARTITION europe VALUES ('europe')
);

-- MPP-5185
-- Should work
CREATE TABLE rank (id int, rank int, year date, gender
char(1)) DISTRIBUTED BY (id, gender, year)
partition by list (gender)
subpartition by range (year)
subpartition template (
start (date '2001-01-01'),
start (date '2002-01-01'),
start (date '2003-01-01'),
start (date '2004-01-01'),
start (date '2005-01-01')
)
(
partition boys values ('M'),
partition girls values ('F')
);

alter table rank set subpartition template ();

-- nothing there
select * from pg_partition_templates;

alter table rank set subpartition template (default subpartition def2);

-- def2 is there
select * from pg_partition_templates;

alter table rank set subpartition template (default subpartition def2);
-- Should still be there
select * from pg_partition_templates;

drop table rank;

-- MPP-5397
-- should be able to add partition after dropped a col

create table mpp_5397 (a int, b int, c int) 
  distributed by (a) 
  partition by range (b)  
  (partition a1 start (0) end (5), 
   partition a2 end (10),  
   partition a3 end(15));

alter table mpp_5397 drop column c;

-- should work now
alter table mpp_5397 add partition z end (20);

drop table mpp_5397;
