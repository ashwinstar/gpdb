-----------------------------------------------------------------------
-- Basic syntax
-- Expect: success
-----------------------------------------------------------------------

prepare ccddlcheck as
select attrelid::regclass as relname,
attnum, attoptions from pg_class c, pg_attribute_encoding e
where c.relname like 'ccddl%' and c.oid=e.attrelid
order by relname, attnum;

-- default encoding clause
create table ccddl (
	i int,
	j int,
	default column encoding (compresstype=zlib)
) with (appendonly=true, orientation=column);

execute ccddlcheck;

-- This is enough to force compression, specially since we'll hash
-- all to a single segment and the values are all the same.
insert into ccddl select 1, 2 from generate_series(1, 100);
select * from ccddl;

drop table ccddl;

-- Make sure we cleaned up after ourselves: should return zero rows
-- We exclude the named tables because they're hang overs from other
-- tests
select * from pg_attribute_encoding where 
	attrelid not in (select oid from pg_class where
	relname = 't3coquicklz' or relname = 'co' or
	relname = 'aocs_new');

-- mix inline and default
create table ccddl (
	i int,
	j int encoding (compresstype=quicklz),
	default column encoding (compresstype=zlib)
) with (appendonly=true, orientation=column);

execute ccddlcheck;

drop table ccddl;

-- mix column reference and default
create table ccddl (
	i int,
	j int,
	default column encoding (compresstype=zlib),
	column j encoding (compresstype=quicklz)
) with (appendonly=true, orientation=column);

execute ccddlcheck;
drop table ccddl;

-- encoding clause for only some columns, others should
-- have no encoding
create table ccddl (
	i int,
	j text encoding (compresstype=quicklz, blocksize=65536, compresslevel=1)
)  with (appendonly=true, orientation=column);

execute ccddlcheck;

-- Should see the encoding information for the new column
alter table ccddl add column k timestamp default now()
	encoding (compresstype=zlib);
execute ccddlcheck;

-- no encoding information for this one though
alter table ccddl add column l numeric
	default 3.141;
execute ccddlcheck;

drop table ccddl;

-- Decipher encoding clause references
create table ccddl (i int, j int encoding(compresstype=zlib), column i encoding
(compresstype=zlib), default column encoding (compresstype=quicklz)) with
(appendonly=true, orientation=column);
execute ccddlcheck;
drop table ccddl;

-- WITH (..., compresstype=<type>) should act as a default clause
create table ccddl (i int, j int) with(appendonly = true, orientation = column,
compresstype=zlib);
execute ccddlcheck;
drop table ccddl;

-- Mix case for compresstype since it should be case insensitive
create table ccddl (i int, j int encoding (COMPRESSTYPE="QuIckLZ"))
	with(appendonly = true, orientation = column);
execute ccddlcheck;
drop table ccddl;

-----------------------------------------------------------------------
-- Basic syntax
-- Expect: failure
-----------------------------------------------------------------------

-- only support CO tables
create table ccddl (i int encoding (compresstype=quicklz));
create table ccddl (i int encoding (compresstype=quicklz))
	with (appendonly = true);

-- can't add encoding to a non-CO table
create table ccddl (i int);
alter table ccddl add column j int encoding (compresstype=quicklz);
drop table ccddl;

create table ccddl (i int) with (appendonly=true);
alter table ccddl add column j int encoding (compresstype=quicklz);
drop table ccddl;

-- encoding clause conflicts with WITH clause
create table a (i int encoding(compresstype=quicklz)) with(compresstype=zlib,
appendonly=true, orientation=column);

-- conflicting WITH and DEFAULT COLUMN ENCODING clause
create table t1 (i int, j int, default column encoding (compresstype=quicklz))
with (compresstype=zlib, appendonly=true, orientation=column);

-- Invalid encoding clauses
create table t1 (i int encoding (compresstype=quicklz, compresslevel=9))
with (appendonly=true, orientation=column);

create table t1 (i int encoding (compresstype=quicklz, ahhhh=boooooo))
with (appendonly=true, orientation=column);

-----------------------------------------------------------------------
-- Partitioning support
-- Expect: success
-----------------------------------------------------------------------

-- trivial partitioning case
create table ccddl (i int, j int)
with (appendonly = true, orientation=column)
partition by range(j)
(partition p1 start(1) end(10),
 partition p2 start(10) end(20),
 column i encoding(compresstype=zlib),
 column j encoding(compresstype=quicklz)
);

execute ccddlcheck;

insert into ccddl select 1,2 from generate_series(1, 100);
select * from ccddl;
drop table ccddl;

-- subpartition template
create table ccddl (i int, j int, k int, l int)
with (appendonly = true, orientation=column)
partition by range(j)
	subpartition by list (k)
	subpartition template(
		subpartition sp1 values(1, 2, 3, 4, 5),
 		column i encoding(compresstype=zlib),
		column j encoding(compresstype=quicklz),
		column k encoding(compresstype=zlib),
		column l encoding(compresstype=zlib)
	)

(partition p1 start(1) end(10),
 partition p2 start(10) end(20)
);

execute ccddlcheck;

select parencattnum, parencattoptions from
pg_partition_encoding e, pg_partition p, pg_class c
where c.relname = 'ccddl' and c.oid = p.parrelid and p.oid = e.parencoid;

insert into ccddl select 1, (i % 19) + 1, ((i+3) % 5) + 1, i+3 from generate_series(1, 100) i;

select * from ccddl;

-- Verify dependency handling
alter table ccddl drop column l;

insert into ccddl select 1, (i % 19) + 1, ((i+3) % 5) + 1 from generate_series(1, 100) i;

select parencattnum, parencattoptions from
pg_partition_encoding e, pg_partition p, pg_class c
where c.relname = 'ccddl' and c.oid = p.parrelid and p.oid = e.parencoid;

select * from ccddl;

drop table ccddl;

-- Add partition should 'inherit' the subpartition template storage encodings
create table ccddl (i int, j int, k int, l int)
with
(appendonly = true, orientation=column)
partition by range(j)
subpartition by list (k)
subpartition template(
subpartition sp1 values(1, 2, 3, 4, 5),
column i encoding(compresstype=zlib),
column j encoding(compresstype=quicklz),
column k encoding(compresstype=zlib),
column l encoding(compresstype=zlib))
(partition p1 start(1) end(10),
partition p2 start(10) end(20)
);

execute ccddlcheck;

alter table ccddl add partition p3 start(20) end(30);

execute ccddlcheck;

drop table ccddl;

-- Make sure the AO/CO case didn't screw up the non CO case
create table ccddl (i int, j int, k int, l int)
with
(appendonly = true)
partition by range(j)
subpartition by list (k)
subpartition template(
subpartition sp1 values(1, 2, 3, 4, 5))
(partition p1 start(1) end(10),
partition p2 start(10) end(20)
);

execute ccddlcheck;

alter table ccddl add partition p3 start(20) end(30);

execute ccddlcheck;

drop table ccddl;

-- Should be nothing in pg_partition_encoding now
select * from pg_partition_encoding;

-- Inheritance: check that we haven't broken anything!
create table ccddlparent (i int encoding (compresstype=zlib))
with (appendonly = true, orientation = column);
create table ccddlchild (j int encoding (compresstype=zlib))
inherits(ccddlparent) with (appendonly = true, orientation = column);

execute ccddlcheck;

insert into ccddlparent select 1 from generate_series(1, 100);
insert into ccddlchild select 1, 2 from generate_series(1, 100);

select * from ccddlparent;
select * from ccddlchild;

drop table ccddlparent cascade;

-----------------------------------------------------------------------
-- Partitioning support
-- Expect: failure
-----------------------------------------------------------------------

-----------------------------------------------------------------------
-- Type support
-- Expect: success
-----------------------------------------------------------------------

-- The basics
drop type if exists int42 cascade;
create type int42;
CREATE FUNCTION int42_in(cstring)
RETURNS int42
AS 'int4in'
LANGUAGE internal IMMUTABLE STRICT;

CREATE FUNCTION int42_out(int42)
RETURNS cstring
AS 'int4out'
LANGUAGE internal IMMUTABLE STRICT;

CREATE TYPE int42 (
internallength = 4,
input = int42_in,
output = int42_out,
alignment = int4,
default = 42,
passedbyvalue,
compresstype="quicklz",
blocksize=65536,
compresslevel=1
);

select typoptions from pg_type_encoding where typid='public.int42'::regtype;

create table ccddl (i int42) with(appendonly = true, orientation=column);
execute ccddlcheck;

alter type int42 set default encoding (compresstype=zlib);
alter table ccddl add column j int42 default '1'::int42;
execute ccddlcheck;

drop table ccddl;

-- Shouldn't apply type default encoding in these cases
create table ccddl (i int42);
execute ccddlcheck;
drop table ccddl;

create table ccddl (i int42) with (appendonly = true);
execute ccddlcheck;
drop table ccddl;

create table ccddl (i int42) with (appendonly = true, orientation=column,
compresstype=none);
execute ccddlcheck;
drop table ccddl;

-----------------------------------------------------------------------
-- Type support
-- Expect: failure
-----------------------------------------------------------------------

-- permissions checks
create role typcheck;
set session authorization typcheck;
alter type int42 set default encoding (compresstype=zlib, compresslevel=1);
reset session authorization;
drop role typcheck;

-- dependency check on drop type
drop type int42 cascade;

select * from pg_type_encoding;

-- cleanup
deallocate ccddlcheck;


-----------------------------------------------------------------------
-- RLE sanity checks
-- Expect: failure
-----------------------------------------------------------------------

-- RLE_TYPE is not supported on AO (non-CO) tables
create table ccddl (i int) with(appendonly = true, compresstype = rle_type);
