-- gpdiffcheck -- check behavior of gpdiff tool

-- 
-- Greenplum Database Statistics Dump
-- Copyright (C) 2007  Greenplum Inc.
-- 
-- Date:     2008-01-28
-- Time:     15:50:52.436688-08
-- Version:  PostgreSQL 8.2.5 (Greenplum Database cvs Head build dev) on i386-apple-darwin8.11.1, compiled by GCC i686-apple-darwin8-gcc-4.0.1 (GCC) 4.0.1 (Apple Computer, Inc. build 5367) compiled on Jan 10 2008 16:26:53 (with assert checking)
-- 
--
-- Greenplum Database database dump
--

SET client_encoding = 'UTF8';
SET standard_conforming_strings = off;
SET check_function_bodies = false;
SET client_min_messages = warning;
SET escape_string_warning = off;

--
-- Name: SCHEMA public; Type: COMMENT; Schema: -; Owner: -
--

COMMENT ON SCHEMA public IS 'Standard public schema';


SET search_path = public, pg_catalog;

SET default_tablespace = '';

SET default_with_oids = false;

--
-- Name: gpd1; Type: TABLE; Schema: public; Owner: -; Tablespace: 
--

CREATE TABLE gpd1 (
    c1 character(1),
    c2 numeric,
    c3 numeric
) distributed by (c1);
--create table gpd1 (c1 char(1), c2 numeric, c3 numeric);
insert into gpd1 values ('a', 1, 1);
insert into gpd1 values ('a', 1, 2);
insert into gpd1 values ('b', 2, 1);
insert into gpd1 values ('b', 1, 2);
insert into gpd1 values ('c', 3, 2);
insert into gpd1 values ('c', 2, 3);
insert into gpd1 values ('d', 4, 4);
insert into gpd1 values ('d', 4, 3);

--
-- Greenplum Database database dump complete
--

-- 
-- Table: gpd1, Attribute: c1
-- 
INSERT INTO pg_statistic VALUES (
	'public.gpd1'::regclass,
	1::smallint,
	0::real,
	2::integer,
	-0.5::real,
	1::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{1}'::real[],
	'{1}'::real[],
	NULL::real[],
	NULL::real[],
	'{b}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: gpd1, Attribute: c2
-- 
INSERT INTO pg_statistic VALUES (
	'public.gpd1'::regclass,
	2::smallint,
	0::real,
	7::integer,
	-0.75::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{1}'::real[],
	NULL::real[],
	'{0}'::real[],
	NULL::real[],
	'{4}'::numeric[],
	'{2,3}'::numeric[],
	NULL::numeric[],
	NULL::numeric[]);

-- 
-- Table: gpd1, Attribute: c3
-- 
INSERT INTO pg_statistic VALUES (
	'public.gpd1'::regclass,
	3::smallint,
	0::real,
	7::integer,
	-1::real,
	2::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	NULL::real[],
	'{0.5}'::real[],
	NULL::real[],
	NULL::real[],
	'{1,2}'::numeric[],
	NULL::numeric[],
	NULL::numeric[],
	NULL::numeric[]);

-- set pages and tuple counts

-- 
-- Table: gpd1
-- 
UPDATE pg_class
SET
	relpages = 1::int,
	reltuples = 2::int
WHERE relname = 'gpd1' AND relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'public');



-- 
-- Greenplum Database Statistics Dump complete
-- 

--
-- ignore
--
select c1 from gpd1;

--
-- order 1, 2
--
select c1, c1, c2, c3 from gpd1 order by 1,2;
--
--
select c1, c1, c2, c3 from gpd1 order by 1,2;-- order 1, 2     , 3    , 4
--
-- ignore
-- order 1, 2
--
select c1, c1, c2, c3 from gpd1 order by 1,2;

--
--  mvd 2,3->1 ; 2,3->4,5
-- order 4
--
select c1, c2, c3, c1, c1, c2 from gpd1 order by 4;

--  Brian: the contents of column 1 are not determined by any other 
--  column -- the column "specifies itself"
--
--  mvd 1->1
--
select row_number() over (), c1, c2, c3 from gpd1;

-- Brian: 1 specifies 2
--  
--
    select -- mvd 1 -> 2
        x,
        row_number() over (partition by x) as y,
        z 
    from (values (1,'A'),(1,'B'),(2,'C'),(2,'D')) r(x,z);

-- start_ignore
--
-- whatever is here is ignored until we reach end_ignore
--
-- end_ignore
--
--
-- explain testing
--
set gp_segments_for_planner=4;
set gp_cost_hashjoin_chainwalk=on;
explain analyze select a.* from gpd1 as a, gpd1 as b where b.c1 in (select max(c1) from gpd1);
explain select a.* from gpd1 as a, gpd1 as b where b.c1 in (select max(c1) from gpd1);
select a.* from gpd1 as a, gpd1 as b where b.c1 in (select max(c1) from gpd1);
set gp_segments_for_planner=40;
explain select a.* from gpd1 as a, gpd1 as b where b.c1 in (select max(c1) from gpd1);
select a.* from gpd1 as a, gpd1 as b where b.c1 in (select max(c1) from gpd1);
-- start_ignore
-- ignore an explain
explain analyze select a.* from gpd1 as a, gpd1 as b where b.c1 in (select max(c1) from gpd1);
-- end_ignore
--
-- start_equiv
--
-- order 1
select c1 from gpd1 order by 1;
--
--
select c1 from gpd1 ;
--
--end_equiv
--
--
--
-- Clean up
--

DROP TABLE  gpd1;

