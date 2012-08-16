--
-- MPP- 18122 - Improve code coverage of the parser 
-- 

drop table if exists mpp_18122_test;
create table mpp_18122_test(id int, ts timestamp, cd date);
insert into mpp_18122_test values(1, timestamp '2004-07-01 13:38:40', date '2012-08-15');

-- extract function
select extract(month from timestamp '2001-02-16 20:38:40') from mpp_18122_test; 
select extract(day from cd) from mpp_18122_test; 
select extract(minute from ts) from mpp_18122_test; 
select extract(second from ts) from mpp_18122_test; 
select extract(month from interval '1year 2 months');
select extract(minute from interval 'P1Y2M3DT4H5M6S');

drop table mpp_18122_test;

-- set session characteristics
show transaction_read_only; 

set session characteristics as transaction read only; 
show transaction_read_only; 

create table mpp_18122_test(a int);

set session characteristics as transaction read write; 
show transaction_read_only; 

create table mpp_18122_test(a int);

drop table mpp_18122_test;

-- client_encoding
show client_encoding;

set client_encoding to SQL_ASCII;

show client_encoding;

reset client_encoding;
show client_encoding;

set NAMES 'SQL_ASCII';

show client_encoding;
reset client_encoding;

-- iso_level 
show transaction_isolation; 
set TRANSACTION ISOLATION LEVEL SERIALIZABLE;

-- alter index 
drop table if exists test_table;
create table test_table(a int, b text default 'abc');
insert into test_table(a) select * from generate_series(1,100);

create index i1 on test_table(a); 

alter index i1 rename to test_table_index;

select t.relname as relname, i.relname as indname from pg_class t, pg_class i, pg_index pi
where t.oid = pi.indrelid and t.relname = 'test_table' 
and i.oid = pi.indexrelid;

drop table test_table; 

-- show session authorization 
drop user if exists mpp_18122_user; 
create role mpp_18122_user with login superuser;

set session authorization mpp_18122_user; 
show session authorization;
reset session authorization;

-- create schema
drop table if exists mpp_18122_tab;
drop schema if exists mpp_18122_schema;

create schema mpp_18122_schema authorization mpp_18122_user create table mpp_18122_tab(a int, b int); 

select c.relname as tabname, n.nspname as namespace 
from pg_class c, pg_namespace n
where c.relname = 'mpp_18122_tab' and c.relnamespace = n.oid;

create schema authorization mpp_18122_user create view temp_view as select * from mpp_18122_schema.mpp_18122_tab; 

drop view mpp_18122_user.temp_view; 
drop table mpp_18122_schema.mpp_18122_tab;
drop schema mpp_18122_schema;

-- set timezone 
set time zone INTERVAL '-08:00' HOUR TO MINUTE;
show time zone; 

set time zone default; 
show time zone; 

set time zone local; 
show time zone; 

set time zone -7;
show time zone; 

