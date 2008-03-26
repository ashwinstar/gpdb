drop table if exists skewed;
drop table if exists spread;

drop table if exists foo;

create table skewed (a int, b int);
create table spread (a int, b int);

set gp_interconnect_type=tcp;

insert into skewed select 1, i from generate_series(1, 1000) i;
insert into spread select i, 1 from generate_series(1, 1000) i;

select * from skewed, spread where skewed.a = spread.b order by 3 desc limit 20;

create table foo as select *, 'Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.'::text as c from skewed;

select * from foo, spread where foo.a = spread.b order by foo.a, spread.a desc, c limit 20;

truncate skewed;
truncate spread;

drop table if exists foo;

set gp_interconnect_type=udp;

insert into skewed select 1, i from generate_series(1, 1000) i;
insert into spread select i, 1 from generate_series(1, 1000) i;

create table foo as select *, 'Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.'::text as c from skewed;

select * from foo, spread where foo.a = spread.b order by foo.a, spread.a desc, c limit 20;

select * from skewed, spread where skewed.a = spread.b order by 3 asc limit 20;
