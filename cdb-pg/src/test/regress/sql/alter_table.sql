--
-- ALTER_TABLE
-- add attribute
--
DROP TABLE IF EXISTS tmp;
CREATE TABLE tmp (initial int4);

COMMENT ON TABLE tmp_wrong IS 'table comment';
COMMENT ON TABLE tmp IS 'table comment';
COMMENT ON TABLE tmp IS NULL;

ALTER TABLE tmp ADD COLUMN a int4 default 3;

ALTER TABLE tmp ADD COLUMN b name;

ALTER TABLE tmp ADD COLUMN c text;

ALTER TABLE tmp ADD COLUMN d float8;

ALTER TABLE tmp ADD COLUMN e float4;

ALTER TABLE tmp ADD COLUMN f int2;

ALTER TABLE tmp ADD COLUMN g polygon;

ALTER TABLE tmp ADD COLUMN h abstime;

ALTER TABLE tmp ADD COLUMN i char;

ALTER TABLE tmp ADD COLUMN j abstime[];

ALTER TABLE tmp ADD COLUMN k int4;

ALTER TABLE tmp ADD COLUMN l tid;

ALTER TABLE tmp ADD COLUMN m xid;

ALTER TABLE tmp ADD COLUMN n oidvector;

--ALTER TABLE tmp ADD COLUMN o lock;
ALTER TABLE tmp ADD COLUMN p smgr;

ALTER TABLE tmp ADD COLUMN q point;

ALTER TABLE tmp ADD COLUMN r lseg;

ALTER TABLE tmp ADD COLUMN s path;

ALTER TABLE tmp ADD COLUMN t box;

ALTER TABLE tmp ADD COLUMN u tinterval;

ALTER TABLE tmp ADD COLUMN v timestamp;

ALTER TABLE tmp ADD COLUMN w interval;

ALTER TABLE tmp ADD COLUMN x float8[];

ALTER TABLE tmp ADD COLUMN y float4[];

ALTER TABLE tmp ADD COLUMN z int2[];

INSERT INTO tmp (a, b, c, d, e, f, g, h, i, j, k, l, m, n, p, q, r, s, t, u,
	v, w, x, y, z)
   VALUES (4, 'name', 'text', 4.1, 4.1, 2, '(4.1,4.1,3.1,3.1)', 
        'Mon May  1 00:30:30 1995', 'c', '{Mon May  1 00:30:30 1995, Monday Aug 24 14:43:07 1992, epoch}', 
	314159, '(1,1)', '512',
	'1 2 3 4 5 6 7 8', 'magnetic disk', '(1.1,1.1)', '(4.1,4.1,3.1,3.1)',
	'(0,2,4.1,4.1,3.1,3.1)', '(4.1,4.1,3.1,3.1)', '["epoch" "infinity"]',
	'epoch', '01:00:10', '{1.0,2.0,3.0,4.0}', '{1.0,2.0,3.0,4.0}', '{1,2,3,4}');

SELECT * FROM tmp;

DROP TABLE tmp;

-- the wolf bug - schema mods caused inconsistent row descriptors 
CREATE TABLE tmp (
	initial 	int4
);

ALTER TABLE tmp ADD COLUMN a int4;

ALTER TABLE tmp ADD COLUMN b name;

ALTER TABLE tmp ADD COLUMN c text;

ALTER TABLE tmp ADD COLUMN d float8;

ALTER TABLE tmp ADD COLUMN e float4;

ALTER TABLE tmp ADD COLUMN f int2;

ALTER TABLE tmp ADD COLUMN g polygon;

ALTER TABLE tmp ADD COLUMN h abstime;

ALTER TABLE tmp ADD COLUMN i char;

ALTER TABLE tmp ADD COLUMN j abstime[];

ALTER TABLE tmp ADD COLUMN k int4;

ALTER TABLE tmp ADD COLUMN l tid;

ALTER TABLE tmp ADD COLUMN m xid;

ALTER TABLE tmp ADD COLUMN n oidvector;

--ALTER TABLE tmp ADD COLUMN o lock;
ALTER TABLE tmp ADD COLUMN p smgr;

ALTER TABLE tmp ADD COLUMN q point;

ALTER TABLE tmp ADD COLUMN r lseg;

ALTER TABLE tmp ADD COLUMN s path;

ALTER TABLE tmp ADD COLUMN t box;

ALTER TABLE tmp ADD COLUMN u tinterval;

ALTER TABLE tmp ADD COLUMN v timestamp;

ALTER TABLE tmp ADD COLUMN w interval;

ALTER TABLE tmp ADD COLUMN x float8[];

ALTER TABLE tmp ADD COLUMN y float4[];

ALTER TABLE tmp ADD COLUMN z int2[];

INSERT INTO tmp (a, b, c, d, e, f, g, h, i, j, k, l, m, n, p, q, r, s, t, u,
	v, w, x, y, z)
   VALUES (4, 'name', 'text', 4.1, 4.1, 2, '(4.1,4.1,3.1,3.1)', 
        'Mon May  1 00:30:30 1995', 'c', '{Mon May  1 00:30:30 1995, Monday Aug 24 14:43:07 1992, epoch}', 
	314159, '(1,1)', '512',
	'1 2 3 4 5 6 7 8', 'magnetic disk', '(1.1,1.1)', '(4.1,4.1,3.1,3.1)',
	'(0,2,4.1,4.1,3.1,3.1)', '(4.1,4.1,3.1,3.1)', '["epoch" "infinity"]',
	'epoch', '01:00:10', '{1.0,2.0,3.0,4.0}', '{1.0,2.0,3.0,4.0}', '{1,2,3,4}');

SELECT * FROM tmp;

DROP TABLE tmp;


--
-- rename - check on both non-temp and temp tables
--
CREATE TABLE tmp (regtable int);
CREATE TEMP TABLE tmp (tmptable int);

ALTER TABLE tmp RENAME TO tmp_new;

SELECT * FROM tmp;
SELECT * FROM tmp_new;

ALTER TABLE tmp RENAME TO tmp_new2;

SELECT * FROM tmp;		-- should fail
SELECT * FROM tmp_new;
SELECT * FROM tmp_new2;

DROP TABLE tmp_new;
DROP TABLE tmp_new2;

-- ALTER TABLE ... RENAME on corrupted relations
SET allow_system_table_mods = dml;
-- missing entry
CREATE TABLE cor (a int, b float);
INSERT INTO cor SELECT i, i+1 FROM generate_series(1,100)i;
DELETE FROM pg_attribute WHERE attname='a' AND attrelid='cor'::regclass;
ALTER TABLE cor RENAME TO oldcor;
INSERT INTO pg_attribute SELECT distinct * FROM gp_dist_random('pg_attribute') WHERE attname='a' AND attrelid='oldcor'::regclass;
DROP TABLE oldcor;

-- typname is out of sync
CREATE TABLE cor (a int, b float, c text);
UPDATE pg_type SET typname='newcor' WHERE typrelid='cor'::regclass;
ALTER TABLE cor RENAME TO newcor;
ALTER TABLE newcor RENAME TO cor;
DROP TABLE cor;

-- relname is out of sync
CREATE TABLE cor (a int, b int);
UPDATE pg_class SET relname='othercor' WHERE relname='cor';
ALTER TABLE othercor RENAME TO tmpcor;
ALTER TABLE tmpcor RENAME TO cor;
DROP TABLE cor;

RESET allow_system_table_mods;

-- ALTER TABLE ... RENAME on non-table relations
-- renaming indexes (FIXME: this should probably test the index's functionality)
ALTER INDEX onek_unique1 RENAME TO tmp_onek_unique1;
ALTER INDEX tmp_onek_unique1 RENAME TO onek_unique1;
-- renaming views
CREATE VIEW tmp_view (unique1) AS SELECT unique1 FROM tenk1;
ALTER TABLE tmp_view RENAME TO tmp_view_new;

-- hack to ensure we get an indexscan here
ANALYZE tenk1;
set enable_seqscan to off;
set enable_bitmapscan to off;
-- 5 values, sorted 
SELECT unique1 FROM tenk1 WHERE unique1 < 5 ORDER BY 1;
reset enable_seqscan;
reset enable_bitmapscan;

DROP VIEW tmp_view_new;

-- FOREIGN KEY CONSTRAINT adding TEST

CREATE TABLE tmp2 (a int primary key);

CREATE TABLE tmp3 (a int, b int);

CREATE TABLE tmp4 (a int, b int, unique(a,b));

CREATE TABLE tmp5 (a int, b int);

-- Insert rows into tmp2 (pktable)
INSERT INTO tmp2 values (1);
INSERT INTO tmp2 values (2);
INSERT INTO tmp2 values (3);
INSERT INTO tmp2 values (4);

-- Insert rows into tmp3
INSERT INTO tmp3 values (1,10);
INSERT INTO tmp3 values (1,20);
INSERT INTO tmp3 values (5,50);

-- Try (and fail) to add constraint due to invalid source columns
ALTER TABLE tmp3 add constraint tmpconstr foreign key(c) references tmp2 match full;

-- Try (and fail) to add constraint due to invalide destination columns explicitly given
ALTER TABLE tmp3 add constraint tmpconstr foreign key(a) references tmp2(b) match full;

-- Try (and fail) to add constraint due to invalid data
ALTER TABLE tmp3 add constraint tmpconstr foreign key (a) references tmp2 match full;

-- Delete one row, add the constraint again -- should fail
DELETE FROM tmp3 where a=5;
ALTER TABLE tmp3 add constraint tmpconstr foreign key (a) references tmp2 match full;

-- Try (and fail) to create constraint from tmp5(a) to tmp4(a) - unique constraint on
-- tmp4 is a,b

ALTER TABLE tmp5 add constraint tmpconstr foreign key(a) references tmp4(a) match full;

DROP TABLE tmp5;

DROP TABLE tmp4;

DROP TABLE tmp3;

DROP TABLE tmp2;

-- Foreign key adding test with mixed types

-- Note: these tables are TEMP to avoid name conflicts when this test
-- is run in parallel with foreign_key.sql.

CREATE TEMP TABLE PKTABLE (ptest1 int PRIMARY KEY);
CREATE TEMP TABLE FKTABLE (ftest1 inet);
-- This next should fail, because inet=int does not exist
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1) references pktable;
-- This should also fail for the same reason, but here we
-- give the column name
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1) references pktable(ptest1);
-- This should succeed, even though they are different types
-- because varchar=int does exist
DROP TABLE FKTABLE;
CREATE TEMP TABLE FKTABLE (ftest1 varchar);
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1) references pktable;
-- As should this
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1) references pktable(ptest1);
DROP TABLE pktable cascade;
DROP TABLE fktable;

CREATE TEMP TABLE PKTABLE (ptest1 int, ptest2 inet,
                           PRIMARY KEY(ptest1, ptest2));
-- This should fail, because we just chose really odd types
CREATE TEMP TABLE FKTABLE (ftest1 cidr, ftest2 timestamp);
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1, ftest2) references pktable;
DROP TABLE FKTABLE;
-- Again, so should this...
CREATE TEMP TABLE FKTABLE (ftest1 cidr, ftest2 timestamp);
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1, ftest2)
     references pktable(ptest1, ptest2);
DROP TABLE FKTABLE;
-- This fails because we mixed up the column ordering
CREATE TEMP TABLE FKTABLE (ftest1 int, ftest2 inet);
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1, ftest2)
     references pktable(ptest2, ptest1);
-- As does this...
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest2, ftest1)
     references pktable(ptest1, ptest2);

-- temp tables should go away by themselves, need not drop them.

-- test check constraint adding

create table atacc1 ( test int );
-- add a check constraint
alter table atacc1 add constraint atacc_test1 check (test>3);
-- should fail
insert into atacc1 (test) values (2);
-- should succeed
insert into atacc1 (test) values (4);
drop table atacc1;

-- let's do one where the check fails when added
create table atacc1 ( test int );
-- insert a soon to be failing row
insert into atacc1 (test) values (2);
-- add a check constraint (fails)
alter table atacc1 add constraint atacc_test1 check (test>3);
insert into atacc1 (test) values (4);
drop table atacc1;

-- let's do one where the check fails because the column doesn't exist
create table atacc1 ( test int );
-- add a check constraint (fails)
alter table atacc1 add constraint atacc_test1 check (test1>3);
drop table atacc1;

-- something a little more complicated
create table atacc1 ( test int, test2 int, test3 int);
-- add a check constraint (fails)
alter table atacc1 add constraint atacc_test1 check (test+test2<test3*4);
-- should fail
insert into atacc1 (test,test2,test3) values (4,4,2);
-- should succeed
insert into atacc1 (test,test2,test3) values (4,4,5);
drop table atacc1;

-- lets do some naming tests
create table atacc1 (test int check (test>3), test2 int);
alter table atacc1 add check (test2>test);
-- should fail for $2
insert into atacc1 (test2, test) values (3, 4);
drop table atacc1;


-- test unique constraint adding

create table atacc1 ( test int ) with oids;
-- add a unique constraint
alter table atacc1 add constraint atacc_test1 unique (test);
-- insert first value
insert into atacc1 (test) values (2);
-- should fail
insert into atacc1 (test) values (2);
-- should succeed
insert into atacc1 (test) values (4);
-- try adding a unique oid constraint
alter table atacc1 add constraint atacc_oid1 unique(oid);
drop table atacc1;

-- let's do one where the unique constraint fails when added
create table atacc1 ( test int );
-- insert soon to be failing rows
insert into atacc1 (test) values (2);
insert into atacc1 (test) values (2);
-- add a unique constraint (fails)
alter table atacc1 add constraint atacc_test1 unique (test);
insert into atacc1 (test) values (3);
drop table atacc1;

-- let's do one where the unique constraint fails
-- because the column doesn't exist
create table atacc1 ( test int );
-- add a unique constraint (fails)
alter table atacc1 add constraint atacc_test1 unique (test1);
drop table atacc1;

-- something a little more complicated
create table atacc1 ( test int, test2 int);
-- add a unique constraint
alter table atacc1 add constraint atacc_test1 unique (test, test2);
-- insert initial value
insert into atacc1 (test,test2) values (4,4);
-- should fail
insert into atacc1 (test,test2) values (4,4);
-- should all succeed
insert into atacc1 (test,test2) values (4,5);
insert into atacc1 (test,test2) values (5,4);
insert into atacc1 (test,test2) values (5,5);
drop table atacc1;

-- lets do some naming tests
create table atacc1 (test int, test2 int, unique(test));
alter table atacc1 add unique (test2);
-- should fail for @@ second one @@
insert into atacc1 (test2, test) values (3, 3);
insert into atacc1 (test2, test) values (2, 3);
drop table atacc1;

-- test primary key constraint adding

create table atacc1 ( test int ) with oids;
-- add a primary key constraint
alter table atacc1 add constraint atacc_test1 primary key (test);
-- insert first value
insert into atacc1 (test) values (2);
-- should fail
insert into atacc1 (test) values (2);
-- should succeed
insert into atacc1 (test) values (4);
-- inserting NULL should fail
insert into atacc1 (test) values(NULL);
-- try adding a second primary key (should fail)
alter table atacc1 add constraint atacc_oid1 primary key(oid);
-- drop first primary key constraint
alter table atacc1 drop constraint atacc_test1 restrict;
-- try adding a primary key on oid (should fail)
alter table atacc1 add constraint atacc_oid1 primary key(oid);
drop table atacc1;

-- let's do one where the primary key constraint fails when added
create table atacc1 ( test int );
-- insert soon to be failing rows
insert into atacc1 (test) values (2);
insert into atacc1 (test) values (2);
-- add a primary key (fails)
alter table atacc1 add constraint atacc_test1 primary key (test);
insert into atacc1 (test) values (3);
drop table atacc1;

-- let's do another one where the primary key constraint fails when added
create table atacc1 ( test int );
-- insert soon to be failing row
insert into atacc1 (test) values (NULL);
-- add a primary key (fails)
alter table atacc1 add constraint atacc_test1 primary key (test);
insert into atacc1 (test) values (3);
drop table atacc1;

-- let's do one where the primary key constraint fails
-- because the column doesn't exist
create table atacc1 ( test int );
-- add a primary key constraint (fails)
alter table atacc1 add constraint atacc_test1 primary key (test1);
drop table atacc1;

-- adding a new column as primary key to a non-empty table.
-- should fail unless the column has a non-null default value.
create table atacc1 ( test int );
insert into atacc1 (test) values (0);
-- add a primary key column without a default (fails).
alter table atacc1 add column test2 int primary key;
-- now add a primary key column with a default (succeeds).
alter table atacc1 add column test2 int default 0 primary key;
drop table atacc1;

-- something a little more complicated
create table atacc1 ( test int, test2 int);
-- add a primary key constraint
alter table atacc1 add constraint atacc_test1 primary key (test, test2);
-- try adding a second primary key - should fail
alter table atacc1 add constraint atacc_test2 primary key (test);
-- insert initial value
insert into atacc1 (test,test2) values (4,4);
-- should fail
insert into atacc1 (test,test2) values (4,4);
insert into atacc1 (test,test2) values (NULL,3);
insert into atacc1 (test,test2) values (3, NULL);
insert into atacc1 (test,test2) values (NULL,NULL);
-- should all succeed
insert into atacc1 (test,test2) values (4,5);
insert into atacc1 (test,test2) values (5,4);
insert into atacc1 (test,test2) values (5,5);
drop table atacc1;

-- lets do some naming tests
create table atacc1 (test int, test2 int, primary key(test));
-- only first should succeed
insert into atacc1 (test2, test) values (3, 3);
insert into atacc1 (test2, test) values (2, 3);
insert into atacc1 (test2, test) values (1, NULL);
drop table atacc1;

-- alter table / alter column [set/drop] not null tests
-- try altering system catalogs, should fail
alter table pg_class alter column relname drop not null;
alter table pg_class alter relname set not null;

-- try altering non-existent table, should fail
alter table non_existent alter column bar set not null;
alter table non_existent alter column bar drop not null;

-- test setting columns to null and not null and vice versa
-- test checking for null values and primary key
create table atacc1 (test int not null) with oids;
alter table atacc1 add constraint "atacc1_pkey" primary key (test);
alter table atacc1 alter column test drop not null;
alter table atacc1 drop constraint "atacc1_pkey";
alter table atacc1 alter column test drop not null;
insert into atacc1 values (null);
alter table atacc1 alter test set not null;
delete from atacc1;
alter table atacc1 alter test set not null;

-- try altering a non-existent column, should fail
alter table atacc1 alter bar set not null;
alter table atacc1 alter bar drop not null;

-- try altering the oid column, should fail
alter table atacc1 alter oid set not null;
alter table atacc1 alter oid drop not null;

-- try creating a view and altering that, should fail
create view myview as select * from atacc1;
alter table myview alter column test drop not null;
alter table myview alter column test set not null;
drop view myview;

drop table atacc1;

-- test setting and removing default values
create table def_test (
	c1	int4 default 5,
	c2	text default 'initial_default'
);
insert into def_test default values;
alter table def_test alter column c1 drop default;
insert into def_test default values;
alter table def_test alter column c2 drop default;
insert into def_test default values;
alter table def_test alter column c1 set default 10;
alter table def_test alter column c2 set default 'new_default';
insert into def_test default values;
select * from def_test order by 1,2;

-- set defaults to an incorrect type: this should fail
alter table def_test alter column c1 set default 'wrong_datatype';
alter table def_test alter column c2 set default 20;

-- set defaults on a non-existent column: this should fail
alter table def_test alter column c3 set default 30;

-- set defaults on views: we need to create a view, add a rule
-- to allow insertions into it, and then alter the view to add
-- a default
create view def_view_test as select * from def_test;
create rule def_view_test_ins as
	on insert to def_view_test
	do instead insert into def_test select new.*;
insert into def_view_test default values;
alter table def_view_test alter column c1 set default 45;
insert into def_view_test default values;
alter table def_view_test alter column c2 set default 'view_default';
insert into def_view_test default values;
select * from def_view_test order by 1,2;

drop rule def_view_test_ins on def_view_test;
drop view def_view_test;
drop table def_test;

-- alter table / drop column tests
-- try altering system catalogs, should fail
alter table pg_class drop column relname;

-- try altering non-existent table, should fail
alter table nosuchtable drop column bar;

-- test dropping columns
create table atacc1 (a int4 not null, b int4, c int4 not null, d int4) with oids;
insert into atacc1 values (1, 2, 3, 4);
alter table atacc1 drop a;
alter table atacc1 drop a;

-- SELECTs
select * from atacc1;
select * from atacc1 order by a;
select * from atacc1 order by "........pg.dropped.1........";
select * from atacc1 group by a;
select * from atacc1 group by "........pg.dropped.1........";
select atacc1.* from atacc1;
select a from atacc1;
select atacc1.a from atacc1;
select b,c,d from atacc1;
select a,b,c,d from atacc1;
select * from atacc1 where a = 1;
select "........pg.dropped.1........" from atacc1;
select atacc1."........pg.dropped.1........" from atacc1;
select "........pg.dropped.1........",b,c,d from atacc1;
select * from atacc1 where "........pg.dropped.1........" = 1;

-- UPDATEs
update atacc1 set a = 3;
update atacc1 set b = 2 where a = 3;
update atacc1 set "........pg.dropped.1........" = 3;
update atacc1 set b = 2 where "........pg.dropped.1........" = 3;

-- INSERTs
insert into atacc1 values (10, 11, 12, 13);
insert into atacc1 values (default, 11, 12, 13);
insert into atacc1 values (11, 12, 13);
insert into atacc1 (a) values (10);
insert into atacc1 (a) values (default);
insert into atacc1 (a,b,c,d) values (10,11,12,13);
insert into atacc1 (a,b,c,d) values (default,11,12,13);
insert into atacc1 (b,c,d) values (11,12,13);
insert into atacc1 ("........pg.dropped.1........") values (10);
insert into atacc1 ("........pg.dropped.1........") values (default);
insert into atacc1 ("........pg.dropped.1........",b,c,d) values (10,11,12,13);
insert into atacc1 ("........pg.dropped.1........",b,c,d) values (default,11,12,13);

-- DELETEs
delete from atacc1 where a = 3;
delete from atacc1 where "........pg.dropped.1........" = 3;
delete from atacc1;

-- try dropping a non-existent column, should fail
alter table atacc1 drop bar;

-- try dropping the oid column, should succeed
alter table atacc1 drop oid;

-- try dropping the xmin column, should fail
alter table atacc1 drop xmin;

-- try creating a view and altering that, should fail
create view myview as select * from atacc1;
select * from myview order by 1,2,3;
alter table myview drop d;
drop view myview;

-- test some commands to make sure they fail on the dropped column
analyze atacc1(a);
analyze atacc1("........pg.dropped.1........");
vacuum analyze atacc1(a);
vacuum analyze atacc1("........pg.dropped.1........");
comment on column atacc1.a is 'testing';
comment on column atacc1."........pg.dropped.1........" is 'testing';
alter table atacc1 alter a set storage plain;
alter table atacc1 alter "........pg.dropped.1........" set storage plain;
alter table atacc1 alter a set statistics 0;
alter table atacc1 alter "........pg.dropped.1........" set statistics 0;
alter table atacc1 alter a set default 3;
alter table atacc1 alter "........pg.dropped.1........" set default 3;
alter table atacc1 alter a drop default;
alter table atacc1 alter "........pg.dropped.1........" drop default;
alter table atacc1 alter a set not null;
alter table atacc1 alter "........pg.dropped.1........" set not null;
alter table atacc1 alter a drop not null;
alter table atacc1 alter "........pg.dropped.1........" drop not null;
alter table atacc1 rename a to x;
alter table atacc1 rename "........pg.dropped.1........" to x;
alter table atacc1 add primary key(a);
alter table atacc1 add primary key("........pg.dropped.1........");
alter table atacc1 add unique(a);
alter table atacc1 add unique("........pg.dropped.1........");
alter table atacc1 add check (a > 3);
alter table atacc1 add check ("........pg.dropped.1........" > 3);
create table atacc2 (id int4 unique);
alter table atacc1 add foreign key (a) references atacc2(id);
alter table atacc1 add foreign key ("........pg.dropped.1........") references atacc2(id);
alter table atacc2 add foreign key (id) references atacc1(a);
alter table atacc2 add foreign key (id) references atacc1("........pg.dropped.1........");
drop table atacc2;
create index "testing_idx" on atacc1(a);
create index "testing_idx" on atacc1("........pg.dropped.1........");

-- test create as and select into
insert into atacc1 values (21, 22, 23);
create table test1 as select * from atacc1;
select * from test1 order by 1,2;
drop table test1;
select * into test2 from atacc1;
select * from test2 order by 1,2;
drop table test2;

-- try dropping all columns
alter table atacc1 drop c;
alter table atacc1 drop d;
alter table atacc1 drop b;
select * from atacc1;

drop table atacc1;

-- test copy in/out
create table test (a int4, b int4, c int4);
insert into test values (1,2,3);
alter table test drop a;
copy test to stdout;
copy test(a) to stdout;
copy test("........pg.dropped.1........") to stdout;
copy test from stdin;
10	11	12
\.
select * from test order by 1;
copy test from stdin;
21	22
\.
select * from test order by 1;
copy test(a) from stdin;
copy test("........pg.dropped.1........") from stdin;
copy test(b,c) from stdin;
31	32
\.
select * from test order by 1;
drop table test;


--
-- Test the ALTER TABLE WITHOUT OIDS command
--
create table altstartwith (col integer) with oids;

insert into altstartwith values (1);

select oid > 0, * from altstartwith;

alter table altstartwith set without oids;

select oid > 0, * from altstartwith; -- fails
select * from altstartwith;

-- test that operations with a dropped column do not try to reference
-- its datatype

create domain mytype as text;
create temp table foo (f1 text, f2 mytype, f3 text);

insert into foo values('aa','bb','cc');
select * from foo;

drop domain mytype cascade;

select * from foo;
insert into foo values('qq','rr');
select * from foo order by 1,2;
update foo set f3 = 'zz';
select * from foo order by 1,2;
select f3,max(f1) from foo group by f3 order by f3;

-- Simple tests for alter table column type
alter table foo alter f1 TYPE integer; -- fails
alter table foo alter f1 TYPE varchar(10);

create table anothertab (atcol1 serial8, atcol2 boolean,
	constraint anothertab_chk check (atcol1 <= 3));

insert into anothertab (atcol1, atcol2) values (default, true);
insert into anothertab (atcol1, atcol2) values (default, false);
select * from anothertab order by 1,2;

alter table anothertab alter column atcol1 type boolean; -- fails
alter table anothertab alter column atcol1 type integer;

select * from anothertab order by 1,2;

insert into anothertab (atcol1, atcol2) values (45, null); -- fails
insert into anothertab (atcol1, atcol2) values (default, null);

select * from anothertab order by 1,2;

alter table anothertab alter column atcol2 type text
      using case when atcol2 is true then 'IT WAS TRUE' 
                 when atcol2 is false then 'IT WAS FALSE'
                 else 'IT WAS NULL!' end;

select * from anothertab order by 1,2;
alter table anothertab alter column atcol1 type boolean
        using case when atcol1 % 2 = 0 then true else false end; -- fails
alter table anothertab alter column atcol1 drop default;
alter table anothertab alter column atcol1 type boolean
        using case when atcol1 % 2 = 0 then true else false end; -- fails
alter table anothertab drop constraint anothertab_chk;

alter table anothertab alter column atcol1 type boolean
        using case when atcol1 % 2 = 0 then true else false end;

select * from anothertab order by 1,2;

drop table anothertab;

create table another (f1 int, f2 text);

insert into another values(1, 'one');
insert into another values(2, 'two');
insert into another values(3, 'three');

select * from another order by 1,2;

alter table another
  alter f1 type text using f2 || ' more',
  alter f2 type bigint using f1 * 10;

select * from another order by 1,2;

drop table another;

--
-- alter function
--
create function test_strict(text) returns text as
    'select coalesce($1, ''got passed a null'');'
    language sql returns null on null input;
select test_strict(NULL);
alter function test_strict(text) called on null input;
select test_strict(NULL);

create function non_strict(text) returns text as
    'select coalesce($1, ''got passed a null'');'
    language sql called on null input;
select non_strict(NULL);
alter function non_strict(text) returns null on null input;
select non_strict(NULL);

--
-- alter object set schema
--

create schema alter1;
create schema alter2;

create table alter1.t1(f1 serial primary key, f2 int check (f2 > 0));

create view alter1.v1 as select * from alter1.t1;

create function alter1.plus1(int) returns int as 'select $1+1' language sql;

create domain alter1.posint integer check (value > 0);

create type alter1.ctype as (f1 int, f2 text);

insert into alter1.t1(f2) values(11);
insert into alter1.t1(f2) values(12);

alter table alter1.t1 set schema alter2;
alter table alter1.v1 set schema alter2;
alter function alter1.plus1(int) set schema alter2;
alter domain alter1.posint set schema alter2;
alter type alter1.ctype set schema alter2;

-- this should succeed because nothing is left in alter1
drop schema alter1;

insert into alter2.t1(f2) values(13);
insert into alter2.t1(f2) values(14);

select * from alter2.t1 order by 1;

select * from alter2.v1 order by 1;

select alter2.plus1(41);

-- clean up
drop schema alter2 cascade;

--
-- Test ALTER TABLE ADD COLUMN WITH NULL DEFAULT on AO TABLES
--
---
--- basic support for alter add column with NULL default to AO tables
--- 
drop table if exists ao1;
create table ao1(col1 varchar(2), col2 int) WITH (APPENDONLY=TRUE) distributed randomly;

insert into ao1 values('aa', 1);
insert into ao1 values('bb', 2);

-- following should be OK.
alter table ao1 add column col3 char(1) default 5;

-- the following should be supported now
alter table ao1 add column col4 char(1) default NULL;

select * from ao1;
insert into ao1 values('cc', 3);
select * from ao1;

alter table ao1 alter column col4 drop default; 
select * from ao1;
insert into ao1 values('dd', 4);
select * from ao1;

---
--- check catalog contents after alter table on AO tables 
---
drop table if exists ao1;
create table ao1(col1 varchar(2), col2 int) WITH (APPENDONLY=TRUE) distributed randomly;

-- relnatts is 2
select relname, relnatts from pg_class where relname = 'ao1';

alter table ao1 add column col3 char(1) default NULL;

-- relnatts in pg_class should be 3
select relname, relnatts from pg_class where relname = 'ao1';

-- check col details in pg_attribute
select  pg_class.relname, attname, typname from pg_attribute, pg_class, pg_type where attrelid = pg_class.oid and pg_class.relname = 'ao1' and atttypid = pg_type.oid and attname = 'col3';

-- no explicit entry in pg_attrdef for NULL default
select relname, attname, adsrc from pg_class, pg_attribute, pg_attrdef where attrelid = pg_class.oid and adrelid = pg_class.oid and adnum = pg_attribute.attnum and pg_class.relname = 'ao1';


--- 
--- check with IS NOT NULL constraint
--- 
drop table if exists ao1;
create table ao1(col1 varchar(2), col2 int) WITH (APPENDONLY=TRUE) distributed randomly;

insert into ao1 values('a', 1); 

-- should fail
alter table ao1 add column col3 char(1) not null default NULL; 

drop table if exists ao1;
create table ao1(col1 varchar(2), col2 int) WITH (APPENDONLY=TRUE) distributed randomly;

-- should pass
alter table ao1 add column col3 char(1) not null default NULL; 

-- this should fail (same behavior as heap tables)
insert into ao1(col1, col2) values('a', 10);
---
--- alter add with no default should continue to fail
---
drop table if exists ao1;
create table ao1(col1 varchar(1)) with (APPENDONLY=TRUE) distributed randomly;

insert into ao1 values('1');
insert into ao1 values('1');
insert into ao1 values('1');
insert into ao1 values('1');

alter table ao1 add column col2 char(1);
select * from ao1;

---
--- new column with a domain type
---
drop table if exists ao1;
create table ao1(col1 varchar(5)) with (APPENDONLY=TRUE) distributed randomly;

insert into ao1 values('abcde');

drop domain zipcode;
create domain zipcode as text
constraint c1 not null;

-- following should fail
alter table ao1 add column col2 zipcode;

alter table ao1 add column col2 zipcode default NULL;

select * from ao1;

-- cleanup
drop table ao1;
drop domain zipcode;


drop schema if exists mpp17582;
create schema mpp17582;
set search_path=mpp17582;

DROP TABLE testbug_char5;
CREATE TABLE testbug_char5
(
timest character varying(6),
user_id numeric(16,0) NOT NULL,
to_be_drop char(5), -- Iterate through different data types
tag1 char(5), -- Iterate through different data types
tag2 char(5)
)
DISTRIBUTED BY (user_id)
PARTITION BY LIST(timest)
(
PARTITION part201203 VALUES('201203') WITH (APPENDONLY=true, COMPRESSLEVEL=5, ORIENTATION=column),
PARTITION part201204 VALUES('201204') WITH (APPENDONLY=true, COMPRESSLEVEL=5, ORIENTATION=row),
PARTITION part201205 VALUES('201205')
);

create index testbug_char5_tag1 on testbug_char5 using btree(tag1);

insert into testbug_char5 (timest,user_id,to_be_drop) select '201203',1111,'10000';
insert into testbug_char5 (timest,user_id,to_be_drop) select '201204',1111,'10000';
insert into testbug_char5 (timest,user_id,to_be_drop) select '201205',1111,'10000';

select * from testbug_char5 order by 1,2;

ALTER TABLE testbug_char5 drop column to_be_drop;

select * from testbug_char5 order by 1,2;

insert into testbug_char5 (timest,user_id,tag2) select '201203',2222,'2';
insert into testbug_char5 (timest,user_id,tag2) select '201204',2222,'2';
insert into testbug_char5 (timest,user_id,tag2) select '201205',2222,'2';

select * from testbug_char5 order by 1,2;

alter table testbug_char5 add PARTITION part201206 VALUES('201206') WITH (APPENDONLY=true, COMPRESSLEVEL=5, ORIENTATION=column);
alter table testbug_char5 add PARTITION part201207 VALUES('201207') WITH (APPENDONLY=true, COMPRESSLEVEL=5, ORIENTATION=row);
alter table testbug_char5 add PARTITION part201208 VALUES('201208');

insert into testbug_char5 select '201206',3333,'1','2';
insert into testbug_char5 select '201207',3333,'1','2';
insert into testbug_char5 select '201208',3333,'1','2';


select * from testbug_char5 order by 1,2;

--
-- Check index scan
--

set enable_seqscan=off;
set enable_indexscan=on;

select * from testbug_char5 where tag1='1';

--
-- Check NL Index scan plan
--

create table dim(tag1 char(5));
insert into dim values('1');

set enable_hashjoin=off;
set enable_seqscan=off;
set enable_nestloop=on;
set enable_indexscan=on;

select * from testbug_char5, dim where testbug_char5.tag1=dim.tag1;

--
-- Load from another table
--

DROP TABLE load;
CREATE TABLE load
(
timest character varying(6),
user_id numeric(16,0) NOT NULL,
tag1 char(5),
tag2 char(5)
)
DISTRIBUTED randomly;

insert into load select '20120' || i , 1111 * (i + 2), '1','2' from generate_series(3,8) i;
select * from load;

insert into testbug_char5 select * from load;

select * from testbug_char5;

--
-- Update values
--

update testbug_char5 set tag1='6' where tag1='1' and timest='201208';
update testbug_char5 set tag2='7' where tag2='1' and timest='201208';

select * from testbug_char5;

set search_path=public;
