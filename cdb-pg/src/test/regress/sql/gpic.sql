drop table if exists skewed;
drop table if exists spread;

drop table if exists foo;

create table skewed (a int, b int);
create table spread (a int, b int);

set gp_interconnect_type=tcp;
set gp_enable_mk_sort = off;

insert into skewed select 1, i from generate_series(1, 1000) i;
insert into spread select i, 1 from generate_series(1, 1000) i;

select * from skewed, spread where skewed.a = spread.b order by 1, 2, 3, 4 desc limit 20;

create table foo as select *, 'Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.'::text as c from skewed;

select foo.a, spread.a, c from foo, spread where foo.a = spread.b order by foo.a, spread.a desc, c limit 20;

truncate skewed;
truncate spread;

drop table if exists foo;

set gp_interconnect_type=udp;

insert into skewed select 1, i from generate_series(1, 1000) i;
insert into spread select i, 1 from generate_series(1, 1000) i;

create table foo as select *, 'Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.'::text as c from skewed;

select foo.a, spread.a, c from foo, spread where foo.a = spread.b order by foo.a, spread.a desc, c limit 20;

select * from skewed, spread where skewed.a = spread.b order by 1 asc, 2, 3 asc, 4 limit 20;

drop table if exists foo;
create table foo (a int, b text);

begin;
set gp_interconnect_type=nil;
insert into foo values (1, 'test');
select * from foo;
abort;

set gp_enable_mk_sort = on;
drop table if exists foo;
