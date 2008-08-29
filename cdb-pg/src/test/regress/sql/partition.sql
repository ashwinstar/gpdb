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

alter table rank DROP partition if exists neuter;

-- can't drop the final partition - must drop the table
alter table rank DROP partition girls;

-- XXX XXX: should work but doesn't
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

drop table rank2 cascade;

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
