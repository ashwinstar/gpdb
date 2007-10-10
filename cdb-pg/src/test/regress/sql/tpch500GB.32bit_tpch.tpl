-- 
-- Greenplum Database Statistics Dump
-- Copyright (C) 2007  Greenplum Inc.
-- 
-- Database: tpch500
-- Date:     2007-10-05
-- Time:     12:07:47.570184-07
-- CmdLine:    
-- Version:  PostgreSQL 8.2.4 (Greenplum Database Release-3_1_0_0-alpha1-branch build 1896) on i386-pc-solaris2.10, compiled by GCC gcc.exe (GCC) 4.1.1 compiled on Sep 25 2007 10:48:21
-- 
--
-- Greenplum Database database dump
--

SET client_encoding = 'SQL_ASCII';
SET standard_conforming_strings = off;
SET check_function_bodies = false;
SET client_min_messages = warning;
SET escape_string_warning = off;

--
-- Name: SCHEMA tpch500gb; Type: COMMENT; Schema: -; Owner: -
--

COMMENT ON SCHEMA public IS 'Standard public schema';
create schema tpch500gb;

SET search_path = tpch500gb, pg_catalog;

SET default_tablespace = '';

SET default_with_oids = false;

--
-- Name: customer; Type: TABLE; Schema: tpch500gb; Owner: -; Tablespace: 
--

CREATE TABLE customer (
    c_custkey integer NOT NULL,
    c_name character varying(25) NOT NULL,
    c_address character varying(40) NOT NULL,
    c_nationkey integer NOT NULL,
    c_phone character(15) NOT NULL,
    c_acctbal numeric(15,2) NOT NULL,
    c_mktsegment character(10) NOT NULL,
    c_comment character varying(117) NOT NULL
) distributed by (c_custkey);


--
-- Name: lineitem; Type: TABLE; Schema: tpch500gb; Owner: -; Tablespace: 
--

CREATE TABLE lineitem (
    l_orderkey bigint NOT NULL,
    l_partkey integer NOT NULL,
    l_suppkey integer NOT NULL,
    l_linenumber integer NOT NULL,
    l_quantity numeric(15,2) NOT NULL,
    l_extendedprice numeric(15,2) NOT NULL,
    l_discount numeric(15,2) NOT NULL,
    l_tax numeric(15,2) NOT NULL,
    l_returnflag character(1) NOT NULL,
    l_linestatus character(1) NOT NULL,
    l_shipdate date NOT NULL,
    l_commitdate date NOT NULL,
    l_receiptdate date NOT NULL,
    l_shipinstruct character(25) NOT NULL,
    l_shipmode character(10) NOT NULL,
    l_comment character varying(44) NOT NULL
) distributed by (l_orderkey);


--
-- Name: nation; Type: TABLE; Schema: tpch500gb; Owner: -; Tablespace: 
--

CREATE TABLE nation (
    n_nationkey integer NOT NULL,
    n_name character(25) NOT NULL,
    n_regionkey integer NOT NULL,
    n_comment character varying(152)
) distributed by (n_nationkey);


--
-- Name: orders; Type: TABLE; Schema: tpch500gb; Owner: -; Tablespace: 
--

CREATE TABLE orders (
    o_orderkey bigint NOT NULL,
    o_custkey integer NOT NULL,
    o_orderstatus character(1) NOT NULL,
    o_totalprice numeric(15,2) NOT NULL,
    o_orderdate date NOT NULL,
    o_orderpriority character(15) NOT NULL,
    o_clerk character(15) NOT NULL,
    o_shippriority integer NOT NULL,
    o_comment character varying(79) NOT NULL
) distributed by (o_orderkey);


--
-- Name: part; Type: TABLE; Schema: tpch500gb; Owner: -; Tablespace: 
--

CREATE TABLE part (
    p_partkey integer NOT NULL,
    p_name character varying(55) NOT NULL,
    p_mfgr character(25) NOT NULL,
    p_brand character(10) NOT NULL,
    p_type character varying(25) NOT NULL,
    p_size integer NOT NULL,
    p_container character(10) NOT NULL,
    p_retailprice numeric(15,2) NOT NULL,
    p_comment character varying(23) NOT NULL
) distributed by (p_partkey);


--
-- Name: partsupp; Type: TABLE; Schema: tpch500gb; Owner: -; Tablespace: 
--

CREATE TABLE partsupp (
    ps_partkey integer NOT NULL,
    ps_suppkey integer NOT NULL,
    ps_availqty integer NOT NULL,
    ps_supplycost numeric(15,2) NOT NULL,
    ps_comment character varying(199) NOT NULL
) distributed by (ps_partkey);


--
-- Name: region; Type: TABLE; Schema: tpch500gb; Owner: -; Tablespace: 
--

CREATE TABLE region (
    r_regionkey integer NOT NULL,
    r_name character(25) NOT NULL,
    r_comment character varying(152)
) distributed by (r_regionkey);


--
-- Name: revenue0; Type: VIEW; Schema: tpch500gb; Owner: -
--

CREATE VIEW revenue0 AS
    SELECT lineitem.l_suppkey AS supplier_no, sum((lineitem.l_extendedprice * ((1)::numeric - lineitem.l_discount))) AS total_revenue FROM lineitem WHERE ((lineitem.l_shipdate >= '1993-01-01'::date) AND (lineitem.l_shipdate < ('1993-01-01'::date + '3 mons'::interval))) GROUP BY lineitem.l_suppkey;


--
-- Name: supplier; Type: TABLE; Schema: tpch500gb; Owner: -; Tablespace: 
--

CREATE TABLE supplier (
    s_suppkey integer NOT NULL,
    s_name character(25) NOT NULL,
    s_address character varying(40) NOT NULL,
    s_nationkey integer NOT NULL,
    s_phone character(15) NOT NULL,
    s_acctbal numeric(15,2) NOT NULL,
    s_comment character varying(101) NOT NULL
) distributed by (s_suppkey);


--
-- Greenplum Database database dump complete
--

-- 
-- Table: customer, Attribute: c_custkey
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.customer'::regclass,
	1::smallint,
	0::real,
	4::integer,
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
	'{1}'::real[],
	NULL::real[],
	NULL::real[],
	'{2760,2945054,6011527,9060392,12127239,15130636,18115629,20965936,23837989,26810261,29946728,32990075,36064800,39101345,42175069,45251547,48278194,51373837,54536725,57520465,60426176,63358487,66267515,69220550,72200435,74998946}'::int4[],
	NULL::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: customer, Attribute: c_name
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.customer'::regclass,
	2::smallint,
	0::real,
	19::integer,
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
	'{1}'::real[],
	NULL::real[],
	NULL::real[],
	'{Customer#000002760,Customer#002945054,Customer#006011527,Customer#009060392,Customer#012127239,Customer#015130636,Customer#018115629,Customer#020965936,Customer#023837989,Customer#026810261,Customer#029946728,Customer#032990075,Customer#036064800,Customer#039101345,Customer#042175069,Customer#045251547,Customer#048278194,Customer#051373837,Customer#054536725,Customer#057520465,Customer#060426176,Customer#063358487,Customer#066267515,Customer#069220550,Customer#072200435,Customer#074998946}'::varchar[],
	NULL::varchar[],
	NULL::varchar[],
	NULL::varchar[]);

-- 
-- Table: customer, Attribute: c_address
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.customer'::regclass,
	3::smallint,
	0::real,
	25::integer,
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
	'{0.00246784}'::real[],
	NULL::real[],
	NULL::real[],
	'{000PrtNXGPu6AD1mL3C7rF8byZnUf,",2LBGg1tjdLDMEKrL1F3n3fOhXFSQA",52RUgseqs6Hx1Gek1,7GhQZTTfPoLEvT0tS80P48qog,"9vTfJ5gUVcY E",BAk5WMgqxNeoDSzx7IcBcRfsizD,"cJ4NubdXQKgv lZmvaxi","DP,sdo9hXUVINV1GnMsRC9,DYXrPvm7hh3Zi1 ","eWMZ4twPD2NxPPIcHiea,kgeK,9Rx8","g5TLLcaDnOqWaO1fEPV BFSmjh2f",hFL7bTEEWywQNSufvX,ilnVMM9j5mTcDD9jj9lciQBY4,jSWGSydRQNrYoKq1MD3jqxu6wqMlU4BEv4,"l1xd7,UvqJkGOLLXMNixh1lleuMY23f08H,",MDrbzstG6ftXWZy,"NKfkHN7aOTIc1C2v,HGkJ9gPw4fCYULjy",oqcNeFKwpvJW1B,PxB3xdvTVT,"RAcK3ic jvB4i5bBFNcoxfed7jG8nxr8vC2yHqwf","SJ3vn6n3dtix Iezl",tQkbZEbvcgCm,"UY43lDGndrB6Z5k5bPSGM 8Bed",wA0coxxPIUekdXKB7TcHfwfAEalpaOtn02KSteJS,"xINctJIdpa4tvwzH76 Ev2a360 g1CL8t WbH vy",yS0FN3zP9Sojt6PeGaUbEZpyhnudk,zZZFzevq0IhGUG7Q9fSp4KsHTaM8JwDQx0}'::varchar[],
	NULL::varchar[],
	NULL::varchar[],
	NULL::varchar[]);

-- 
-- Table: customer, Attribute: c_nationkey
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.customer'::regclass,
	4::smallint,
	0::real,
	4::integer,
	25::real,
	1::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.0419882,0.0412688,0.0410399,0.0410072,0.0409745,0.0409418,0.0408437,0.040811,0.0404186,0.0402878,0.0402551,0.0399935,0.0398954,0.0398627,0.0397646,0.0397646,0.0397319,0.0395356,0.0394702,0.039274,0.0390451,0.0390451,0.03862,0.0384565,0.0377044}'::real[],
	'{0.0426999}'::real[],
	NULL::real[],
	NULL::real[],
	'{21,5,15,4,17,23,13,20,12,18,7,6,9,8,11,22,24,2,1,14,3,10,16,19,0}'::int4[],
	NULL::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: customer, Attribute: c_phone
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.customer'::regclass,
	5::smallint,
	0::real,
	16::integer,
	-0.997547::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{6.54022e-05}'::real[],
	NULL::real[],
	'{0.0027482}'::real[],
	NULL::real[],
	'{27-299-964-7166}'::bpchar[],
	'{10-100-654-9201,11-152-700-2185,12-170-250-8992,13-178-565-3823,14-183-851-4728,15-174-550-1265,16-149-503-8812,17-143-109-8176,18-141-448-3137,19-145-703-8058,20-144-608-1076,21-163-370-8699,22-171-967-2302,23-168-927-9879,24-156-331-7970,25-157-764-3206,26-146-942-2576,27-163-172-2742,28-145-479-8630,29-138-410-3301,30-171-857-5391,31-148-188-8380,32-109-937-9756,33-122-842-6192,33-992-235-7081,34-998-912-4164}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: customer, Attribute: c_acctbal
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.customer'::regclass,
	6::smallint,
	0::real,
	8::integer,
	-0.106222::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{9.81033e-05,9.81033e-05,9.81033e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05}'::real[],
	NULL::real[],
	'{-0.00315596}'::real[],
	NULL::real[],
	'{422.87,5428.98,7655.56,-981.26,-930.14,-886.88,-880.30,-840.89,-823.44,-815.97,-814.52,-807.14,-800.51,-777.54,-724.42,-724.00,-711.43,-699.81,-681.00,-663.98,-644.37,-580.80,-567.29,-557.09,-518.66}'::numeric[],
	'{-999.48,-562.86,-101.69,361.76,802.33,1243.28,1676.83,2134.63,2565.07,2985.87,3415.98,3860.11,4312.80,4742.20,5185.75,5641.98,6084.42,6532.69,6972.31,7406.22,7849.66,8284.99,8714.67,9149.30,9556.33,9999.94}'::numeric[],
	NULL::numeric[],
	NULL::numeric[]);

-- 
-- Table: customer, Attribute: c_mktsegment
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.customer'::regclass,
	7::smallint,
	0::real,
	11::integer,
	5::real,
	1::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.20242,0.200916,0.199804,0.198888,0.197973}'::real[],
	'{0.199686}'::real[],
	NULL::real[],
	NULL::real[],
	'{"BUILDING  ","HOUSEHOLD ","FURNITURE ","MACHINERY ",AUTOMOBILE}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: customer, Attribute: c_comment
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.customer'::regclass,
	8::smallint,
	0::real,
	73::integer,
	-0.973481::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{6.54022e-05}'::real[],
	NULL::real[],
	'{-0.00225504}'::real[],
	NULL::real[],
	'{"ithely across the permanently"}'::varchar[],
	'{" about the accounts. always even deposits haggle blithely bold deposits.","al deposits wake slyly special, silent accounts. slyly express foxes sleep alongside of the ironic packages. "," beans. requests haggle furiously furiously unusual plate","carefully unusual sheaves doze blithely. quickly pending requests against the ironic package","deposits boost evenly fluffy foxes. doggedly ironic packages sleep never above the ","efully even requests haggle carefully. furiously ironic forges boost furiously ruthlessly stealthy instruction","eposits was carefully slyly bold theodolites. furiously ironic foxes alongside of the carefully fina","even requests. boldly special deposits int","fully final packages sleep blithely. fluffily thin foxe","haggle blithely around the quickly final accounts. carefully bo","ilent accounts. regular, pending foxes promise; blithely special deposits dazzle furiously. pending deposits de"," ironic theodolites are furiously. r","le furiously according to the car","ly final pinto beans. packages wake final, regular courts. quickly special pinto beans are b","ngside of the even accounts. regular, final excuses affix carefu","ole alongside of the slyly even theodolites. even braids sleep carefully. final epitaphs could have to w"," packages along the furiously express theodolites haggle slyly fluffily ironic pinto beans. thin, ex","quickly final ideas about the ruthlessly eve","rhorses are fluffily bold excuses. carefully regular instructions across the blithely even pa","s. final, regular deposits wake blithely busy theodolites. carefully"," special, ironic theodolites. fluffily "," the blithely ironic instructions sleep thinly around the ideas. ironic, even asy","ts cajole at the carefully express requests. slyly ironic instructions hang according to the final theodol","unts according to the unusual, bold accounts ca","xes solve carefully among the quickly pe","zzle regular accounts: bold packages cajole furiously alongside of the theodo"}'::varchar[],
	NULL::varchar[],
	NULL::varchar[]);

-- 
-- Table: lineitem, Attribute: l_orderkey
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	1::smallint,
	0::real,
	8::integer,
	1.51038e+06::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{9.81033e-05,9.81033e-05,9.81033e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05}'::real[],
	NULL::real[],
	'{1}'::real[],
	NULL::real[],
	'{4179463,881445987,2962849606,5701541,10170112,22614212,22751876,46360803,60191969,75968416,76845987,108103845,116328161,117470528,122215206,170967363,189143555,189589639,199817094,206086401,216050503,218893280,226045191,230156099,231007654}'::int8[],
	'{257378,127538502,249665444,372152864,498015904,606438274,723059264,842039520,954102753,1073122116,1190333057,1312125350,1436023396,1552490503,1665862306,1785420610,1912523104,2036041376,2151790470,2270244934,2390748865,2515629825,2634774885,2752993505,2881395106,2999929955}'::int8[],
	NULL::int8[],
	NULL::int8[]);

-- 
-- Table: lineitem, Attribute: l_partkey
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	2::smallint,
	0::real,
	4::integer,
	-0.18676::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05}'::real[],
	NULL::real[],
	'{0.00205893}'::real[],
	NULL::real[],
	'{10485043,34678418,34818586,71936778,86724326,98612016,99821045}'::int4[],
	'{1025,3915152,7979591,12003964,16213070,20252426,24305267,28233865,32237899,36274771,40266435,44196081,48219329,52402386,56599376,60535939,64497129,68644684,72573578,76612708,80619158,84426070,88319084,92226041,96048052,99998727}'::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: lineitem, Attribute: l_suppkey
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	3::smallint,
	0::real,
	4::integer,
	5.02283e+06::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05}'::real[],
	NULL::real[],
	'{-0.000392946}'::real[],
	NULL::real[],
	'{58896,97303,240385,261996,271316,356699,397337,422246,474378,505037,540293,593262,618959,660138,679459,802625,835089,905882,958564,968057,1077093,1144432,1151965,1239477,1254999}'::int4[],
	'{116,207592,395468,592116,798217,1003478,1205752,1411909,1609425,1810780,2011000,2212397,2421930,2622802,2825386,3026020,3225675,3422656,3619036,3830614,4031631,4219989,4422712,4620940,4808444,4999810}'::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: lineitem, Attribute: l_linenumber
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	4::smallint,
	0::real,
	4::integer,
	7::real,
	1::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.252289,0.215991,0.179071,0.145618,0.104022,0.067724,0.0352845}'::real[],
	'{0.175633}'::real[],
	NULL::real[],
	NULL::real[],
	'{1,2,3,4,5,6,7}'::int4[],
	NULL::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: lineitem, Attribute: l_quantity
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	5::smallint,
	0::real,
	7::integer,
	50::real,
	2::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	NULL::real[],
	'{0.0209242}'::real[],
	NULL::real[],
	NULL::real[],
	'{1.00,2.00,4.00,6.00,8.00,10.00,12.00,15.00,17.00,18.00,21.00,22.00,24.00,26.00,28.00,30.00,32.00,35.00,36.00,38.00,41.00,42.00,44.00,47.00,48.00,50.00}'::numeric[],
	NULL::numeric[],
	NULL::numeric[],
	NULL::numeric[]);

-- 
-- Table: lineitem, Attribute: l_extendedprice
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	6::smallint,
	0::real,
	10::integer,
	2.53851e+06::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05}'::real[],
	NULL::real[],
	'{0.000138415}'::real[],
	NULL::real[],
	'{1145.20,1211.18,3240.00,3651.30,4501.32,5087.28,5108.34,5476.92,5501.16,5680.08,6197.10,6367.20,7180.64,7595.55,7719.20,7940.70,8792.35,9656.24,9702.35,11024.73,11144.32,11548.50,13155.03,13549.20,14404.91}'::numeric[],
	'{911.28,3562.64,6395.10,9387.36,12300.30,15182.97,18059.02,20988.60,23914.80,26616.48,29473.58,32471.63,35334.20,38281.32,41209.35,44158.53,46816.88,49702.48,52694.22,55953.97,59714.68,63666.85,68447.00,73931.47,81866.25,103165.00}'::numeric[],
	NULL::numeric[],
	NULL::numeric[]);

-- 
-- Table: lineitem, Attribute: l_discount
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	7::smallint,
	0::real,
	6::integer,
	11::real,
	1::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.093656,0.0928058,0.0923806,0.0922171,0.0921517,0.0911707,0.0906802,0.0894375,0.0893067,0.088293,0.0879006}'::real[],
	'{0.0879589}'::real[],
	NULL::real[],
	NULL::real[],
	'{0.02,0.00,0.09,0.03,0.07,0.06,0.05,0.08,0.10,0.04,0.01}'::numeric[],
	NULL::numeric[],
	NULL::numeric[],
	NULL::numeric[]);

-- 
-- Table: lineitem, Attribute: l_tax
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	8::smallint,
	0::real,
	6::integer,
	9::real,
	1::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.113604,0.113277,0.11259,0.112198,0.111576,0.110007,0.109908,0.108829,0.108012}'::real[],
	'{0.112964}'::real[],
	NULL::real[],
	NULL::real[],
	'{0.08,0.06,0.04,0.02,0.07,0.01,0.05,0.03,0.00}'::numeric[],
	NULL::numeric[],
	NULL::numeric[],
	NULL::numeric[]);

-- 
-- Table: lineitem, Attribute: l_returnflag
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	9::smallint,
	0::real,
	2::integer,
	3::real,
	1::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.507423,0.246828,0.245749}'::real[],
	'{0.376786}'::real[],
	NULL::real[],
	NULL::real[],
	'{N,R,A}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: lineitem, Attribute: l_linestatus
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	10::smallint,
	0::real,
	2::integer,
	2::real,
	1::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.501177,0.498823}'::real[],
	'{0.502906}'::real[],
	NULL::real[],
	NULL::real[],
	'{O,F}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: lineitem, Attribute: l_shipdate
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	11::smallint,
	0::real,
	4::integer,
	2510.88::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.000981033,0.000817528,0.000817528,0.000817528,0.000784827,0.000784827,0.000752126,0.000752126,0.000752126,0.000752126,0.000752126,0.000752126,0.000752126,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000686723}'::real[],
	NULL::real[],
	'{0.0029223}'::real[],
	NULL::real[],
	'{1997-03-04,1996-05-16,1997-05-05,1998-03-18,1995-05-04,1997-08-25,1992-12-04,1993-05-09,1993-07-20,1994-02-20,1995-02-23,1997-07-17,1998-05-22,1993-02-23,1993-11-21,1994-04-04,1995-07-25,1996-01-11,1997-01-22,1997-03-18,1997-05-14,1998-01-20,1998-04-22,1998-05-16,1992-04-27}'::date[],
	'{1992-01-03,1992-06-03,1992-09-07,1992-12-17,1993-03-23,1993-06-24,1993-09-27,1994-01-01,1994-04-09,1994-07-14,1994-10-20,1995-01-24,1995-04-29,1995-08-06,1995-11-08,1996-02-11,1996-05-13,1996-08-14,1996-11-21,1997-02-27,1997-06-02,1997-09-08,1997-12-14,1998-03-22,1998-06-27,1998-11-29}'::date[],
	NULL::date[],
	NULL::date[]);

-- 
-- Table: lineitem, Attribute: l_commitdate
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	12::smallint,
	0::real,
	4::integer,
	2458.88::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.000817528,0.000784827,0.000784827,0.000784827,0.000752126,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000686723,0.000686723,0.000686723,0.000686723,0.000686723}'::real[],
	NULL::real[],
	'{0.00314841}'::real[],
	NULL::real[],
	'{1993-10-05,1995-04-30,1997-06-24,1998-05-03,1994-10-01,1992-06-20,1992-10-22,1993-01-09,1993-01-28,1993-05-09,1993-09-22,1993-10-30,1994-01-15,1994-07-14,1994-12-04,1995-11-06,1995-12-04,1996-01-05,1996-05-18,1996-05-28,1992-10-18,1993-04-20,1993-07-08,1993-08-14,1993-09-04}'::date[],
	'{1992-01-31,1992-06-01,1992-09-07,1992-12-18,1993-03-25,1993-07-01,1993-10-09,1994-01-11,1994-04-18,1994-07-27,1994-11-01,1995-02-06,1995-05-12,1995-08-17,1995-11-22,1996-02-25,1996-05-29,1996-09-01,1996-12-10,1997-03-14,1997-06-13,1997-09-15,1997-12-17,1998-03-24,1998-06-27,1998-10-30}'::date[],
	NULL::date[],
	NULL::date[]);

-- 
-- Table: lineitem, Attribute: l_receiptdate
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	13::smallint,
	0::real,
	4::integer,
	2518::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.000850229,0.000850229,0.000850229,0.000817528,0.000784827,0.000784827,0.000784827,0.000784827,0.000784827,0.000752126,0.000752126,0.000752126,0.000752126,0.000752126,0.000752126,0.000752126,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000686723}'::real[],
	NULL::real[],
	'{0.0029204}'::real[],
	NULL::real[],
	'{1993-05-25,1993-12-30,1997-06-16,1994-12-07,1992-05-17,1992-10-05,1993-12-05,1997-06-17,1998-01-02,1992-12-23,1993-02-07,1993-05-22,1994-11-27,1995-04-15,1997-04-24,1998-06-02,1992-09-25,1993-09-28,1994-04-23,1996-01-08,1996-04-21,1996-06-03,1996-09-21,1996-12-13,1992-08-11}'::date[],
	'{1992-01-06,1992-06-18,1992-09-27,1993-01-07,1993-04-13,1993-07-17,1993-10-20,1994-01-26,1994-05-02,1994-08-07,1994-11-12,1995-02-22,1995-05-25,1995-08-28,1995-12-02,1996-03-05,1996-06-07,1996-09-10,1996-12-19,1997-03-25,1997-06-27,1997-09-30,1998-01-06,1998-04-10,1998-07-12,1998-12-23}'::date[],
	NULL::date[],
	NULL::date[]);

-- 
-- Table: lineitem, Attribute: l_shipinstruct
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	14::smallint,
	0::real,
	26::integer,
	4::real,
	1::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.252681,0.251014,0.249902,0.246403}'::real[],
	'{0.253223}'::real[],
	NULL::real[],
	NULL::real[],
	'{"TAKE BACK RETURN         ","DELIVER IN PERSON        ","COLLECT COD              ","NONE                     "}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: lineitem, Attribute: l_shipmode
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	15::smallint,
	0::real,
	11::integer,
	7::real,
	1::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.146599,0.14467,0.143296,0.143296,0.142675,0.141857,0.137606}'::real[],
	'{0.14049}'::real[],
	NULL::real[],
	NULL::real[],
	'{"MAIL      ","AIR       ","RAIL      ","TRUCK     ","SHIP      ","FOB       ","REG AIR   "}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: lineitem, Attribute: l_comment
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.lineitem'::regclass,
	16::smallint,
	0::real,
	27::integer,
	1.06815e+06::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.000228908,0.000196207,0.000196207,0.000163506,0.000163506,0.000163506,0.000163506,0.000163506,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05}'::real[],
	NULL::real[],
	'{-0.0035911}'::real[],
	NULL::real[],
	'{" furiously "," carefully","ly regular ",". carefull"," carefully ","furiously ","ly express",theodolite," accounts "," dependencie"," deposits "," furiously"," furiously final","ly pending ","ly regular"," the careful"," the carefully ","ular package"," accounts a","alongside o","along the ","al request","arefully ironi"," blithely ",". blithely"}'::varchar[],
	'{"about the accoun","al deposits use ","beans lose. carefully special th"," carefully slyly furious instructions"," deposits.","efully final foxes. ironic","equests bo",". even pinto beans impress; slyly r","fully. furiously ironic pinto ","haggle alongside of the qu","ies. excuses nag slyly. foxes ",", ironic t","le. fluffily final reques","ly final asympt","ngside of the final foxes. express de","olites. unusual re","packages are furiously a"," quickly final deposits. r","r ideas wake slyly. platelets solve qu","s. furiously p","special packages use slyly. regular ","the blithely express ","ts about the special, special ","unts affix blithely: slyly bold foxes boos","xcuses. sile","zzle slyly! blithely dogge"}'::varchar[],
	NULL::varchar[],
	NULL::varchar[]);

-- 
-- Table: nation, Attribute: n_nationkey
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.nation'::regclass,
	1::smallint,
	0::real,
	4::integer,
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
	'{1}'::real[],
	NULL::real[],
	NULL::real[],
	'{2,10,18}'::int4[],
	NULL::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: nation, Attribute: n_name
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.nation'::regclass,
	2::smallint,
	0::real,
	26::integer,
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
	'{0.9375}'::real[],
	NULL::real[],
	NULL::real[],
	'{"BRAZIL                   ","CHINA                    ","IRAN                     "}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: nation, Attribute: n_regionkey
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.nation'::regclass,
	3::smallint,
	0::real,
	4::integer,
	-0.802083::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.5}'::real[],
	NULL::real[],
	'{0.4875}'::real[],
	NULL::real[],
	'{0}'::int4[],
	'{1,2,4}'::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: nation, Attribute: n_comment
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.nation'::regclass,
	4::smallint,
	0::real,
	75::integer,
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
	'{-0.025}'::real[],
	NULL::real[],
	NULL::real[],
	'{"c dependencies. furiously express notornis sleep slyly regular accounts. ideas sleep. depos","efully alongside of the slyly final dependencies. ","y alongside of the pending deposits. carefully special packages are about the ironic forges. slyly special "}'::varchar[],
	NULL::varchar[],
	NULL::varchar[],
	NULL::varchar[]);

-- 
-- Table: orders, Attribute: o_orderkey
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.orders'::regclass,
	1::smallint,
	0::real,
	8::integer,
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
	'{1}'::real[],
	NULL::real[],
	NULL::real[],
	'{135842,125830560,254495872,369371105,492624005,603852548,717054784,833980225,954132775,1070058564,1187525569,1310870084,1435700294,1552567651,1666974470,1788841922,1908328864,2019913088,2150803584,2267912039,2389854948,2517943655,2640203783,2753003488,2881133573,2999659940}'::int8[],
	NULL::int8[],
	NULL::int8[],
	NULL::int8[]);

-- 
-- Table: orders, Attribute: o_custkey
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.orders'::regclass,
	2::smallint,
	0::real,
	4::integer,
	-0.330924::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05}'::real[],
	NULL::real[],
	'{-0.00224339}'::real[],
	NULL::real[],
	'{803971,2570255,2865043,21367789,23937334,25923748,29462047,46477165,46797262,55436575,68295988}'::int4[],
	'{3556,3062281,6026218,9013847,12170441,15269131,18215608,21129634,24060460,27111226,30226390,33116704,36119890,39256591,42120659,45205612,48134675,51044593,54061187,56984572,60021268,62880848,65877919,68888008,71934134,74995108}'::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: orders, Attribute: o_orderstatus
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.orders'::regclass,
	3::smallint,
	0::real,
	2::integer,
	3::real,
	1::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.489536,0.484205,0.026259}'::real[],
	'{0.476401}'::real[],
	NULL::real[],
	NULL::real[],
	'{O,F,P}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: orders, Attribute: o_totalprice
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.orders'::regclass,
	4::smallint,
	0::real,
	10::integer,
	-0.26316::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05}'::real[],
	NULL::real[],
	'{-0.00300164}'::real[],
	NULL::real[],
	'{56565.25,65644.26,69889.06,73534.82,75290.93,77703.87,82745.90,106163.32,113323.76,115164.53,120515.80,151751.92,167621.38,169238.67,182924.39,202112.80,205801.62,219773.41,275045.04,275489.57}'::numeric[],
	'{891.43,18372.38,32782.42,44606.06,54820.18,65747.83,75916.31,86050.36,96224.14,107229.85,117765.27,128022.62,138656.20,149755.26,160957.53,171384.43,182980.32,194682.96,206550.55,219096.50,232151.05,247014.85,264672.42,284967.60,314064.87,478313.80}'::numeric[],
	NULL::numeric[],
	NULL::numeric[]);

-- 
-- Table: orders, Attribute: o_orderdate
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.orders'::regclass,
	5::smallint,
	0::real,
	4::integer,
	2406::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.000948332,0.000850229,0.000817528,0.000817528,0.000784827,0.000784827,0.000784827,0.000784827,0.000784827,0.000752126,0.000752126,0.000752126,0.000752126,0.000752126,0.000752126,0.000752126,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424,0.000719424}'::real[],
	NULL::real[],
	'{0.00167432}'::real[],
	NULL::real[],
	'{1993-09-22,1992-08-22,1994-11-01,1997-04-24,1992-01-21,1992-06-23,1994-07-03,1997-01-27,1997-02-13,1993-01-26,1996-02-25,1996-09-05,1997-05-08,1997-05-28,1997-06-29,1997-07-07,1992-10-31,1993-05-09,1993-12-19,1995-05-07,1995-09-19,1996-06-04,1996-06-26,1997-09-13,1997-09-26}'::date[],
	'{1992-01-01,1992-04-13,1992-07-21,1992-10-25,1993-01-31,1993-05-08,1993-08-12,1993-11-12,1994-02-13,1994-05-18,1994-08-24,1994-11-29,1995-03-05,1995-06-07,1995-09-10,1995-12-14,1996-03-14,1996-06-17,1996-09-24,1996-12-29,1997-04-04,1997-07-17,1997-10-17,1998-01-18,1998-04-25,1998-08-02}'::date[],
	NULL::date[],
	NULL::date[]);

-- 
-- Table: orders, Attribute: o_orderpriority
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.orders'::regclass,
	6::smallint,
	0::real,
	16::integer,
	5::real,
	1::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.203303,0.20157,0.201373,0.200458,0.193296}'::real[],
	'{0.197264}'::real[],
	NULL::real[],
	NULL::real[],
	'{"2-HIGH         ","5-LOW          ","1-URGENT       ","3-MEDIUM       ","4-NOT SPECIFIED"}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: orders, Attribute: o_clerk
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.orders'::regclass,
	7::smallint,
	0::real,
	16::integer,
	493057::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05}'::real[],
	NULL::real[],
	'{0.000784734}'::real[],
	NULL::real[],
	'{Clerk#000016630,Clerk#000030432,Clerk#000085909,Clerk#000113201,Clerk#000113345,Clerk#000118072,Clerk#000131280,Clerk#000149090,Clerk#000157847,Clerk#000234455,Clerk#000314809,Clerk#000338736,Clerk#000399991,Clerk#000487719,Clerk#000000770,Clerk#000002379,Clerk#000003678,Clerk#000003967,Clerk#000004123,Clerk#000004505,Clerk#000004511,Clerk#000004598,Clerk#000004940,Clerk#000005937,Clerk#000006101}'::bpchar[],
	'{Clerk#000000024,Clerk#000020411,Clerk#000040622,Clerk#000060909,Clerk#000079988,Clerk#000099554,Clerk#000120040,Clerk#000139870,Clerk#000160802,Clerk#000180384,Clerk#000201092,Clerk#000221847,Clerk#000241518,Clerk#000260835,Clerk#000281358,Clerk#000302063,Clerk#000320865,Clerk#000341341,Clerk#000361327,Clerk#000381445,Clerk#000402205,Clerk#000422065,Clerk#000441873,Clerk#000461301,Clerk#000481429,Clerk#000499997}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: orders, Attribute: o_shippriority
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.orders'::regclass,
	8::smallint,
	0::real,
	4::integer,
	1::real,
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
	'{0}'::int4[],
	NULL::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: orders, Attribute: o_comment
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.orders'::regclass,
	9::smallint,
	0::real,
	49::integer,
	-0.343534::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05}'::real[],
	NULL::real[],
	'{-0.00203013}'::real[],
	NULL::real[],
	'{" carefully regular ","e carefully regular ","e slyly above the furi","ests. furiously unusual "," fluffily. carefully ","ly regular accounts"," quickly regular pack","refully even requests ","refully regular instructi","thely final accounts"}'::varchar[],
	'{" about the blithely even pa","al dolphins wake closely final "," beans wake quickly pending courts. slyly","ccording to the regular sauternes","deposits doze above the regular packag","efully final foxes unwind evenly. furiousl","equests! carefully regular ideas cajole fl"," excuses. furiously ironi","furious deposits. accounts nag blithely final pinto beans. reque","haggle slyly among the deposits. ironic asymptotes mainta","imes express packages. furiously final theodolites about the regula","ithely final requests. silent, express notornis wake daringly blithely even ac","l instructions nag furiously pending accou","ly ironic packages: blithely bold dependencies after the slyly ironic p","nic epitaphs use final, final instructions. fluffily","onic deposits across the always regular","packages! slyly idle pinto beans after the furious","quickly unusual deposits haggle fur","riously along the even, bold instructions. blithely ","s. furiously final accounts boost furiously after th","special deposits haggle regular pinto beans. special asymptotes","tes. pinto beans haggle carefull","ts affix furiously after the regular excuses! regular theod","unts are quickly pending packa","xes boost carefully. bold excuses integrat","zzle idly. regular platelets sleep special, even packages. express pains "}'::varchar[],
	NULL::varchar[],
	NULL::varchar[]);

-- 
-- Table: part, Attribute: p_partkey
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.part'::regclass,
	1::smallint,
	0::real,
	4::integer,
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
	'{1}'::real[],
	NULL::real[],
	NULL::real[],
	'{6250,4000819,7809731,11966397,16105639,20014459,23985244,27837027,31746767,35760273,40118719,44164819,48163631,52078894,56092394,60189315,64200952,68180273,72249368,76254401,80180787,84231746,88055285,92056593,95862943,99997434}'::int4[],
	NULL::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: part, Attribute: p_name
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.part'::regclass,
	2::smallint,
	0::real,
	34::integer,
	-0.996749::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{6.54022e-05}'::real[],
	NULL::real[],
	'{-0.00221096}'::real[],
	NULL::real[],
	'{"cornsilk lemon pink sienna chocolate"}'::varchar[],
	'{"almond antique aquamarine slate powder","aquamarine white peach goldenrod floral","bisque pink frosted sky peru","blue moccasin black ivory azure","burlywood indian rose grey sky","chiffon grey blanched hot salmon","cornflower forest dark lavender goldenrod","cyan honeydew firebrick floral smoke","dim magenta coral khaki chocolate","firebrick sienna rosy almond olive","gainsboro coral papaya burnished yellow","green peach chocolate sienna magenta","indian brown red gainsboro cornsilk","lace navy beige peru saddle","light gainsboro wheat antique orchid","maroon chartreuse lemon mint peru","midnight tomato cornsilk navajo medium","navajo violet lavender blush medium","orchid tan midnight lemon deep","pink blue ivory sienna turquoise","purple deep misty papaya thistle","royal magenta lemon black drab","sienna chartreuse royal lemon lace","snow violet plum thistle deep","turquoise azure black goldenrod lavender","yellow white moccasin olive lace"}'::varchar[],
	NULL::varchar[],
	NULL::varchar[]);

-- 
-- Table: part, Attribute: p_mfgr
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.part'::regclass,
	3::smallint,
	0::real,
	26::integer,
	5::real,
	1::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.20363,0.201864,0.201635,0.197482,0.195389}'::real[],
	'{0.201775}'::real[],
	NULL::real[],
	NULL::real[],
	'{"Manufacturer#1           ","Manufacturer#3           ","Manufacturer#2           ","Manufacturer#5           ","Manufacturer#4           "}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: part, Attribute: p_brand
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.part'::regclass,
	4::smallint,
	0::real,
	11::integer,
	25::real,
	1::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.0423152,0.0421517,0.0421517,0.0410726,0.0410399,0.0408764,0.0408764,0.0405821,0.0402551,0.040157,0.0400589,0.0399935,0.0399608,0.0399281,0.0399281,0.0398954,0.0397973,0.0396337,0.0394702,0.0393721,0.0386854,0.0386854,0.0386854,0.0375082,0.0369196}'::real[],
	'{0.0417735}'::real[],
	NULL::real[],
	NULL::real[],
	'{"Brand#35  ","Brand#23  ","Brand#34  ","Brand#13  ","Brand#12  ","Brand#15  ","Brand#21  ","Brand#14  ","Brand#32  ","Brand#55  ","Brand#11  ","Brand#25  ","Brand#43  ","Brand#24  ","Brand#42  ","Brand#44  ","Brand#53  ","Brand#33  ","Brand#52  ","Brand#54  ","Brand#22  ","Brand#41  ","Brand#51  ","Brand#31  ","Brand#45  "}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: part, Attribute: p_type
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.part'::regclass,
	5::smallint,
	0::real,
	21::integer,
	150::real,
	2::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	NULL::real[],
	'{0.00566047}'::real[],
	NULL::real[],
	NULL::real[],
	'{"ECONOMY ANODIZED BRASS","ECONOMY BRUSHED BRASS","ECONOMY BURNISHED NICKEL","ECONOMY PLATED STEEL","ECONOMY POLISHED TIN","LARGE ANODIZED TIN","LARGE BURNISHED BRASS","LARGE PLATED COPPER","LARGE POLISHED NICKEL","MEDIUM ANODIZED STEEL","MEDIUM BRUSHED TIN","MEDIUM PLATED BRASS","MEDIUM POLISHED COPPER","PROMO ANODIZED NICKEL","PROMO BRUSHED STEEL","PROMO BURNISHED TIN","PROMO POLISHED BRASS","SMALL ANODIZED COPPER","SMALL BRUSHED NICKEL","SMALL BURNISHED STEEL","SMALL PLATED TIN","STANDARD ANODIZED BRASS","STANDARD BRUSHED COPPER","STANDARD BURNISHED NICKEL","STANDARD PLATED TIN","STANDARD POLISHED TIN"}'::varchar[],
	NULL::varchar[],
	NULL::varchar[],
	NULL::varchar[]);

-- 
-- Table: part, Attribute: p_size
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.part'::regclass,
	6::smallint,
	0::real,
	4::integer,
	50::real,
	2::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	NULL::real[],
	'{0.0176015}'::real[],
	NULL::real[],
	NULL::real[],
	'{1,2,4,6,8,11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,46,49,50}'::int4[],
	NULL::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: part, Attribute: p_container
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.part'::regclass,
	7::smallint,
	0::real,
	11::integer,
	40::real,
	2::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	NULL::real[],
	'{0.0244857}'::real[],
	NULL::real[],
	NULL::real[],
	'{"JUMBO BAG ","JUMBO BOX ","JUMBO CASE","JUMBO DRUM","JUMBO PACK","JUMBO PKG ","LG BOX    ","LG CAN    ","LG DRUM   ","LG PACK   ","LG PKG    ","MED BOX   ","MED CASE  ","MED DRUM  ","MED PACK  ","MED PKG   ","SM BOX    ","SM CASE   ","SM DRUM   ","SM PACK   ","SM PKG    ","WRAP BOX  ","WRAP CASE ","WRAP DRUM ","WRAP PACK ","WRAP PKG  "}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: part, Attribute: p_retailprice
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.part'::regclass,
	8::smallint,
	0::real,
	8::integer,
	108231::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,9.81033e-05,9.81033e-05,9.81033e-05}'::real[],
	NULL::real[],
	'{-0.00236066}'::real[],
	NULL::real[],
	'{1101.64,1108.72,1163.49,1205.57,1211.49,1223.15,1244.61,1279.51,1288.61,1343.46,1390.37,1399.48,1402.78,1442.21,1522.65,1578.83,1596.00,1614.27,1640.13,1703.24,1871.39,1893.13,962.63,995.43,998.12}'::numeric[],
	'{901.69,1025.18,1079.57,1121.71,1161.21,1201.87,1240.51,1280.17,1322.24,1363.07,1402.55,1441.56,1480.65,1519.58,1559.51,1600.45,1640.16,1680.82,1721.64,1761.54,1800.69,1840.97,1880.56,1923.57,1974.88,2097.12}'::numeric[],
	NULL::numeric[],
	NULL::numeric[]);

-- 
-- Table: part, Attribute: p_comment
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.part'::regclass,
	9::smallint,
	0::real,
	14::integer,
	107088::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.000555919,0.000523218,0.000523218,0.000490517,0.000490517,0.000490517,0.000457816,0.000457816,0.000457816,0.000457816,0.000425114,0.000425114,0.000425114,0.000425114,0.000425114,0.000425114,0.000392413,0.000392413,0.000392413,0.000392413,0.000392413,0.000392413,0.000392413,0.000392413,0.000392413}'::real[],
	NULL::real[],
	'{-0.000241861}'::real[],
	NULL::real[],
	'{arefu,"slyly "," the ","gular ","ously ",regula,arefully,final," furiou"," furiously",carefull,"iously "," regular",riously,"ular ","usly ",accou,carefu," carefull",carefully,egula,"fully ",iousl,regul," regular "}'::varchar[],
	'{" abou","al deposits. blithe","beans against the","ccording to the ","deposits. carefully","efully sile","equests must slee"," excuses wake. ca",furious,"he even platele","inal pla","its affix blit","lites. furiously bo","lyly regular",nments,"onic pinto bean"," pearls d",rding,"rls. bold ideas caj","silent theodol","ss deposits. car"," the fina","ts. even acco","unts. regular i","xes! slyly iro","zzle fluffily "}'::varchar[],
	NULL::varchar[],
	NULL::varchar[]);

-- 
-- Table: partsupp, Attribute: ps_partkey
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.partsupp'::regclass,
	1::smallint,
	0::real,
	4::integer,
	1.82026e+06::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05}'::real[],
	NULL::real[],
	'{1}'::real[],
	NULL::real[],
	'{690507,715354,942077,1097818,1509826,1557227,1772223,2070199,2383643,2594463,2627268,3472458,4587189,4698277,4811698,4860845,4926970,5685774,5701717,5990983,6759553,6764637,7471778,7632253,7737436}'::int4[],
	'{10394,4192244,8254089,12476628,16454636,20383946,24544532,28430751,32276497,36007599,40066263,44099760,48138505,51864278,56153785,60233322,64130261,68243362,72176000,76281262,80063671,84349540,88262217,92034382,96077321,99999282}'::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: partsupp, Attribute: ps_suppkey
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.partsupp'::regclass,
	2::smallint,
	0::real,
	4::integer,
	-3.77952e+29::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05}'::real[],
	NULL::real[],
	'{0.00637007}'::real[],
	NULL::real[],
	'{127740,183850,267099,303567,446268,475347,535210,571547,672188,699903,700066,704858,776949,800789,931268,1027677,1030631,1032942,1159299,1282535,1369957,1378699,1405642,1410632,1478959}'::int4[],
	'{72,201790,408268,608919,812281,1005496,1210488,1413622,1605971,1811285,2008372,2208839,2414904,2612056,2813837,3013848,3214308,3407922,3604458,3798315,3997473,4195663,4383864,4585279,4787755,4999955}'::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: partsupp, Attribute: ps_availqty
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.partsupp'::regclass,
	3::smallint,
	0::real,
	4::integer,
	9991::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.000359712,0.000359712,0.000359712,0.000359712,0.000359712,0.000327011,0.000327011,0.000327011,0.000327011,0.00029431,0.00029431,0.00029431,0.00029431,0.00029431,0.00029431,0.00029431,0.00029431,0.00029431,0.00029431,0.00029431,0.00029431,0.00029431,0.00029431,0.00029431,0.00029431}'::real[],
	NULL::real[],
	'{0.00147852}'::real[],
	NULL::real[],
	'{1240,4112,4413,5082,6546,3041,3944,5517,7119,178,237,877,1072,1102,2893,3405,3448,3612,4084,4339,4484,4818,4833,4872,5063}'::int4[],
	'{1,400,804,1193,1579,1981,2398,2791,3191,3607,3994,4375,4787,5192,5595,5999,6381,6794,7184,7614,8010,8407,8806,9204,9603,9999}'::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: partsupp, Attribute: ps_supplycost
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.partsupp'::regclass,
	4::smallint,
	0::real,
	8::integer,
	100036::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804,0.000130804}'::real[],
	NULL::real[],
	'{0.000799782}'::real[],
	NULL::real[],
	'{36.32,63.19,64.53,84.78,143.31,158.50,172.27,193.92,262.13,402.73,403.95,520.33,534.64,541.75,593.60,629.54,650.24,663.13,704.38,759.75,759.86,812.28,827.68,828.04,840.97}'::numeric[],
	'{1.02,43.26,82.85,122.13,162.19,201.33,244.02,283.83,323.13,362.65,404.25,443.79,484.65,525.39,563.92,603.22,643.99,682.46,722.92,763.04,802.08,842.82,881.82,920.80,960.81,1000.00}'::numeric[],
	NULL::numeric[],
	NULL::numeric[]);

-- 
-- Table: partsupp, Attribute: ps_comment
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.partsupp'::regclass,
	5::smallint,
	0::real,
	125::integer,
	-0.856298::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{6.54022e-05,6.54022e-05}'::real[],
	NULL::real[],
	'{-0.00105641}'::real[],
	NULL::real[],
	'{" silent deposits haggle furiously. carefully even requests haggle blithely blithely express pac","ular accounts according to the blithely final sheaves use "}'::varchar[],
	'{"about the accounts. furiously unusual foxes according to the ironic instructions haggle quickly across the blithely regular accounts. bold, unusual pin","al, final foxes. slyly unusual theodolites are according to the regular forges. bold accounts according to the regular dependencies are furiously bold braids; slyly pending requests hag"," beans. unusual foxes haggle about the furiously final dolphins. quickly final ideas cajole deposits. ironic,","ccording to the even, ironic requests. carefully even warhorses boost quickly across the pac","deposits. furiously ironic attainments believe furiously regular accounts. fu","efully regular ideas nag slyly blithely final platelets. fluffily special courts above the pinto beans detect slyly ironic pinto beans. blithely final deposits cajole. fluffily ","equests sleep quickly. blithely regular theodolites boost quic","express accounts. carefully regular deposits use carefully. fluffily ironic platelets cajole. thinly unusual ideas are slyly. slyly special packag","furiously even accounts. ironic packages cajole fluffily special excuses. final pinto beans wake fluffily along the bold, unusual requests. ironic accounts cajole slyly. slyly bold foxes aft","he bold pinto beans. ironic, thin pinto beans haggle furiously packages. blithely quick packages above the accounts haggle around the express, iron","inal braids. quickly slow frets against the even accounts haggle carefully along the blithely re","ithely. quickly ironic accounts around the instructions affix fluffily after the s","l, ironic deposits are carefully regular packages. ironic instructions wake furiously across the furi","lyly careful depths use according to the furiously pending packages. boldly silent pains affix. regular theodolites cajole express, bold somas. furious","nic pinto beans. theodolites are even asymptotes. ironic, pending ideas use quickly. dogged pains maintain ironic dinos. theodolites play along the bold, express accounts. unusual pinto beans s","onic foxes sleep carefully pinto beans. waters across the theodolites nag idly express, final asymptotes. ideas are slyly. r","packages wake slyly. even requests sleep. slyly pending instructions sleep across the","r accounts detect quietly furiously special deposits. even frets boost according to the ironic accounts. finally ironic frets integrate blithely ev","riously final instructions; silent instructions at the enticingly pending realms cajole express theodolites. carefully even requ","s haggle slyly carefully regular ideas. furiously unusual pinto beans across the r","special requests use carefully unusual ideas. carefully even instructions use carefully pending pinto beans. regularly bold packages aga","the carefully final accounts. carefully final asymptotes hag","ts. carefully ironic requests are packages. ironically pending dependencies doze according to the packages: i","ultipliers. regular accounts boost slyly about the blithely unusual pinto beans. carefully regular deposits above the quickly regular foxes boost regular ","xes. blithely sly instructions wake furiously among the fluffily final i","zzle slyly. carefully pending accounts integrate furiously above the carefully pending requests. carefully final accounts wake special deposits. express, express accounts haggle "}'::varchar[],
	NULL::varchar[],
	NULL::varchar[]);

-- 
-- Table: region, Attribute: r_regionkey
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.region'::regclass,
	1::smallint,
	0::real,
	4::integer,
	-1::real,
	0::smallint,
	0::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	NULL::real[],
	NULL::real[],
	NULL::real[],
	NULL::real[],
	NULL::int4[],
	NULL::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: region, Attribute: r_name
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.region'::regclass,
	2::smallint,
	0::real,
	26::integer,
	-1::real,
	0::smallint,
	0::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	NULL::real[],
	NULL::real[],
	NULL::real[],
	NULL::real[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: region, Attribute: r_comment
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.region'::regclass,
	3::smallint,
	0::real,
	67::integer,
	-1::real,
	0::smallint,
	0::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	NULL::real[],
	NULL::real[],
	NULL::real[],
	NULL::real[],
	NULL::varchar[],
	NULL::varchar[],
	NULL::varchar[],
	NULL::varchar[]);

-- 
-- Table: supplier, Attribute: s_suppkey
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.supplier'::regclass,
	1::smallint,
	0::real,
	4::integer,
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
	'{1}'::real[],
	NULL::real[],
	NULL::real[],
	'{10,205652,402150,600898,807062,1015479,1211703,1409071,1609511,1801722,1994605,2184849,2378177,2572987,2772271,2965322,3181104,3389360,3589264,3791035,3987227,4189112,4400530,4601227,4802715,4999999}'::int4[],
	NULL::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: supplier, Attribute: s_name
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.supplier'::regclass,
	2::smallint,
	0::real,
	26::integer,
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
	'{1}'::real[],
	NULL::real[],
	NULL::real[],
	'{"Supplier#000000010       ","Supplier#000205652       ","Supplier#000402150       ","Supplier#000600898       ","Supplier#000807062       ","Supplier#001015479       ","Supplier#001211703       ","Supplier#001409071       ","Supplier#001609511       ","Supplier#001801722       ","Supplier#001994605       ","Supplier#002184849       ","Supplier#002378177       ","Supplier#002572987       ","Supplier#002772271       ","Supplier#002965322       ","Supplier#003181104       ","Supplier#003389360       ","Supplier#003589264       ","Supplier#003791035       ","Supplier#003987227       ","Supplier#004189112       ","Supplier#004400530       ","Supplier#004601227       ","Supplier#004802715       ","Supplier#004999999       "}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: supplier, Attribute: s_address
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.supplier'::regclass,
	3::smallint,
	0::real,
	25::integer,
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
	'{0.000266612}'::real[],
	NULL::real[],
	NULL::real[],
	'{"001xxu1BdA2O8XkARdFXp QrNq5CgHm6X7Z","2lNmwj,KV,rmngtT1ZzPnZrRhmGjHJBSo",51GkBojCHZFY,7LbWMpx6bIC5FtZD1C,9yNET7nxlBDVVGtGkRXTLpJtOfKE4,"baQx0vc0fPM8eXfZls f8B","Cj2NhY7FWFFQbOoTkPEE,cjH 4UO","DraOUFosTFiieBce8vM9,xzAyLCJ"," EwbMfb0G5vo4W",g8eDxtznm9gxr3hsgSKrHuXnR5xu,HGnnE5ZecTK,iNVuAAuEvqjRp5INg17B32rsYBsRZNaxcc,"jX5N LEsRuXP",L5W2B1byG9T0Sq7LV2Aupbfx,"MfjKTZYgCdrY,o2LWm4OJv yY0 5","NmRd82NmYHS0FE 7L4oSi",oT9VzHELTXsrjLtw,q3aiJdU1eyy,"RDkdX9QMBeRARCE0qbMwXQnFig,hrQMER",sL7plmJY3T83l6p5qB1dbG6dv8fLjF5ehwEBh,TU7oLNYH3fMIgll2,V2IePdQln1,"wckjsZqrsk8O3AQdx 8nN58Jbey eZB3ks DS","XkOuuwgXTFUkeof,weqyH3Splr0D9","ySH tOSC2h","Z ZYzistXDo8Gnticx,O2KtAItgEz9GXrCKBupst"}'::varchar[],
	NULL::varchar[],
	NULL::varchar[],
	NULL::varchar[]);

-- 
-- Table: supplier, Attribute: s_nationkey
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.supplier'::regclass,
	4::smallint,
	0::real,
	4::integer,
	25::real,
	1::smallint,
	3::smallint,
	0::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{0.0417266,0.0413996,0.041138,0.0408764,0.0406802,0.0406148,0.0406148,0.0406148,0.0405167,0.040484,0.0403859,0.0401243,0.0399608,0.0399281,0.0399281,0.0397973,0.0394375,0.0393721,0.0393067,0.0392413,0.0389797,0.0389797,0.0388489,0.0387508,0.038293}'::real[],
	'{0.0409596}'::real[],
	NULL::real[],
	NULL::real[],
	'{6,15,17,9,12,14,19,24,1,8,18,10,5,22,23,0,3,7,13,20,2,16,11,21,4}'::int4[],
	NULL::int4[],
	NULL::int4[],
	NULL::int4[]);

-- 
-- Table: supplier, Attribute: s_phone
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.supplier'::regclass,
	5::smallint,
	0::real,
	16::integer,
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
	'{0.000825367}'::real[],
	NULL::real[],
	NULL::real[],
	'{10-102-743-1474,11-104-701-8188,11-994-693-2477,13-115-462-2414,14-124-416-6291,15-159-880-6008,16-164-963-3093,17-137-258-3438,18-143-110-5353,19-132-851-7767,20-112-274-9248,21-110-149-6268,22-140-339-4888,23-115-258-4068,24-133-761-3909,25-120-560-7381,25-995-595-7556,27-110-175-9581,27-979-467-7147,28-981-151-1176,29-972-816-4106,30-980-456-1875,32-107-866-9510,33-112-415-3142,34-108-265-6279,34-995-770-2457}'::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[],
	NULL::bpchar[]);

-- 
-- Table: supplier, Attribute: s_acctbal
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.supplier'::regclass,
	6::smallint,
	0::real,
	8::integer,
	-0.647033::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{9.81033e-05,9.81033e-05,9.81033e-05,9.81033e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05}'::real[],
	NULL::real[],
	'{0.00372881}'::real[],
	NULL::real[],
	'{899.59,5672.67,6980.79,9140.75,-989.17,-985.86,-936.90,-887.82,-883.72,-839.83,-838.09,-823.73,-790.22,-768.74,-730.83,-706.27,-675.41,-673.62,-670.56,-664.67,-643.31,-580.80,-555.74,-552.79,-549.02}'::numeric[],
	'{-999.92,-552.62,-88.05,337.41,789.48,1230.13,1665.57,2091.68,2523.05,2949.87,3392.00,3829.08,4279.92,4716.57,5164.06,5628.24,6060.58,6476.44,6918.70,7357.09,7803.14,8220.78,8670.21,9105.56,9565.76,9999.98}'::numeric[],
	NULL::numeric[],
	NULL::numeric[]);

-- 
-- Table: supplier, Attribute: s_comment
-- 
INSERT INTO pg_statistic VALUES (
	'tpch500gb.supplier'::regclass,
	7::smallint,
	0::real,
	63::integer,
	-0.996915::real,
	1::smallint,
	2::smallint,
	3::smallint,
	0::smallint,
	0::oid,
	0::oid,
	0::oid,
	0::oid,
	'{6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05,6.54022e-05}'::real[],
	NULL::real[],
	'{-0.00180453}'::real[],
	NULL::real[],
	'{"ar notornis above the furiously even platelet"," ironic theodolites. carefully regular pinto","ly regular deposits. furiou","refully ironic ideas. carefully special deposits integrate b","uickly against the pending requests. furiously pending ideas hinder a"}'::varchar[],
	'{" about the accounts? quickly final ideas","al dugouts after the slyly unusual accounts kindle blithely after the furiously unusual deposits."," beans sleep ironic foxes. final packages wake about the ironic instructions? bravely special t","ccounts. accounts integrate since the platelets. regular pinto bea"," deposits nag among the quickly regular accounts? idly final exc","efully final instructions. blithe","equests haggle furiously slyly speci"," excuses engage carefully requests? accounts sleep furiously. deposits will na"," furious deposits haggle furiously regular accounts. blithely final deposits sleep among the even, ","he accounts. blithely pendi","inal excuses sleep furiously final dolphins. ","its above the blithely special","lites. unusual packages hang fluffily express theodolites. slyl","lyly pending accounts are blithely after t","nic requests nag slyly after the quickly final requests. packag","onic platelets. sheaves along the regular deposits boost quic","p blithely close platelets. blithely ironic foxes detect slyly a","r dolphins are furiously. even requests across the furiously speci","ronic accounts! furiously pend","s integrate slyly pending deposits. slyly dogged platelets above the blithely ironic pinto beans sle","ss accounts sleep. slyly express instructions cajole blithely after the slyly ironic "," the carefully express depths. special, unusual pinto beans haggle slyly blithe accounts","ts. asymptotes solve fluffily slyly ironic decoys. slyly even theodolites among the regular ","unts after the fluffily ironic accounts engage slyly throughout ","xes. blithely regular ideas wake. unusual packages detect busily: slyly bold th","zzle slyly. platelets are fluffily. bold pinto b"}'::varchar[],
	NULL::varchar[],
	NULL::varchar[]);

-- set pages and tuple counts

-- 
-- Table: nation
-- 
UPDATE pg_class
SET
	relpages = 1::int,
	reltuples = 4::int
WHERE relname = 'nation' AND relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'tpch500gb');

-- 
-- Table: region
-- 
UPDATE pg_class
SET
	relpages = 1::int,
	reltuples = 1::int
WHERE relname = 'region' AND relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'tpch500gb');

-- 
-- Table: part
-- 
UPDATE pg_class
SET
	relpages = 64362::int,
	reltuples = 1.25003e+07::int
WHERE relname = 'part' AND relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'tpch500gb');

-- 
-- Table: supplier
-- 
UPDATE pg_class
SET
	relpages = 3474::int,
	reltuples = 625001::int
WHERE relname = 'supplier' AND relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'tpch500gb');

-- 
-- Table: partsupp
-- 
UPDATE pg_class
SET
	relpages = 275403::int,
	reltuples = 5.00039e+07::int
WHERE relname = 'partsupp' AND relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'tpch500gb');

-- 
-- Table: customer
-- 
UPDATE pg_class
SET
	relpages = 56110::int,
	reltuples = 9.37546e+06::int
WHERE relname = 'customer' AND relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'tpch500gb');

-- 
-- Table: orders
-- 
UPDATE pg_class
SET
	relpages = 427353::int,
	reltuples = 9.37548e+07::int
WHERE relname = 'orders' AND relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'tpch500gb');

-- 
-- Table: lineitem
-- 
UPDATE pg_class
SET
	relpages = 1880625::int,
	reltuples = 3.75023e+08::int
WHERE relname = 'lineitem' AND relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'tpch500gb');

-- 
-- Table: revenue0
-- 
UPDATE pg_class
SET
	relpages = 0::int,
	reltuples = 0::int
WHERE relname = 'revenue0' AND relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'tpch500gb');



-- 
-- Greenplum Database Statistics Dump complete
-- 


set gp_segments_for_planner = 16;


--
--
-- 01.txt
--
--
-- using 2133894592 as a seed to the RNG


select 'query 01' as current_query;

explain select
	l_returnflag,
	l_linestatus,
	sum(l_quantity) as sum_qty,
	sum(l_extendedprice) as sum_base_price,
	sum(l_extendedprice * (1 - l_discount)) as sum_disc_price,
	sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) as sum_charge,
	avg(l_quantity) as avg_qty,
	avg(l_extendedprice) as avg_price,
	avg(l_discount) as avg_disc,
	count(*) as count_order
from
	lineitem
where
	l_shipdate <= date '1998-12-01' - interval '106 day'
group by
	l_returnflag,
	l_linestatus
order by
	l_returnflag,
	l_linestatus;

--
--
-- 02.txt
--
--
-- using 2133894592 as a seed to the RNG

select 'query 02' as current_query;

explain select 
	s.s_acctbal,
	s.s_name,
	n.n_name,
	p.p_partkey,
	p.p_mfgr,
	s.s_address,
	s.s_phone,
	s.s_comment 
from 
	supplier s, 
	partsupp ps, 
	nation n, 
	region r, 
	part p, 
	(select p_partkey, min(ps_supplycost) as min_ps_cost 
		from 
			part, 
			partsupp , 	
			supplier,
			nation, 
			region 
		where 
			p_partkey=ps_partkey 
			and s_suppkey = ps_suppkey 
			and s_nationkey = n_nationkey 
			and n_regionkey = r_regionkey 
			and r_name = 'EUROPE' 
		group by p_partkey ) g 
where 
	p.p_partkey = ps.ps_partkey 
	and g.p_partkey = p.p_partkey 
	and g. min_ps_cost = ps.ps_supplycost 
	and s.s_suppkey = ps.ps_suppkey 
	and p.p_size = 36 
	and p.p_type like '%COPPER' 
	and s.s_nationkey = n.n_nationkey 
	and n.n_regionkey = r.r_regionkey 
	and r.r_name = 'EUROPE' 
order by 
	s.s_acctbal desc,
	n.n_name,
	s.s_name,
	p.p_partkey
LIMIT 100;
--
--
-- 03.txt
--
--
select 'query 03' as current_query;

-- using 2133894592 as a seed to the RNG
explain select
	l_orderkey,
	sum(l_extendedprice * (1 - l_discount)) as revenue,
	o_orderdate,
	o_shippriority
from
	customer,
	orders,
	lineitem
where
	c_mktsegment = 'AUTOMOBILE'
	and c_custkey = o_custkey
	and l_orderkey = o_orderkey
	and o_orderdate < date '1995-03-22'
	and l_shipdate > date '1995-03-22'
group by
	l_orderkey,
	o_orderdate,
	o_shippriority
order by
	revenue desc,
	o_orderdate
LIMIT 10;
--
--
-- 04.txt
--
--
select 'query 04' as current_query;

-- using 2133894592 as a seed to the RNG

explain select
        o_orderpriority,
        count(*) as order_count
from
        orders
where
        o_orderdate >= date '1996-07-01'
        and o_orderdate < date '1996-07-01' + interval '3 month'
        and exists (
                select
                        *
                from
                        lineitem
                where
                        l_orderkey = o_orderkey
                        and l_commitdate < l_receiptdate
        )
group by
        o_orderpriority
order by
        o_orderpriority;

--
--
-- 05.txt
--
--
select 'query 05' as current_query;

-- using 2133894592 as a seed to the RNG

explain select
count(*)
--	n_name,
--	sum(l_extendedprice * (1 - l_discount)) as revenue
from
	customer,
	orders,
	lineitem,
	supplier,
	nation,
	region
where
	c_custkey = o_custkey
	and l_orderkey = o_orderkey
	and l_suppkey = s_suppkey
	and c_nationkey = s_nationkey
	and s_nationkey = n_nationkey
	and n_regionkey = r_regionkey
	and r_name = 'ASIA'
	and o_orderdate >= date '1993-01-01'
	and o_orderdate < date '1993-01-01' + interval '1 year'
--group by
--	n_name
--order by
--	revenue desc
;

--
--
-- 06.txt
--
--
select 'query 06' as current_query;

-- using 2133894592 as a seed to the RNG
explain select
	sum(l_extendedprice * l_discount) as revenue
from
	lineitem
where
	l_shipdate >= date '1993-01-01'
	and l_shipdate < date '1993-01-01' + interval '1 year'
	and l_discount between 0.02 - 0.01 and 0.02 + 0.01
	and l_quantity < 25;

--
--
-- 07.txt
--
--
select 'query 07' as current_query;

-- using 2133894592 as a seed to the RNG
explain select
	supp_nation,
	cust_nation,
	l_year,
	sum(volume) as revenue
from
	(
		select
			n1.n_name as supp_nation,
			n2.n_name as cust_nation,
			extract(year from l_shipdate) as l_year,
			l_extendedprice * (1 - l_discount) as volume
		from
			supplier,
			lineitem,
			orders,
			customer,
			nation n1,
			nation n2
		where
			s_suppkey = l_suppkey
			and o_orderkey = l_orderkey
			and c_custkey = o_custkey
			and s_nationkey = n1.n_nationkey
			and c_nationkey = n2.n_nationkey
			and (
				(n1.n_name = 'ALGERIA' and n2.n_name = 'ROMANIA')
				or (n1.n_name = 'ROMANIA' and n2.n_name = 'ALGERIA')
			)
			and l_shipdate between date '1995-01-01' and date '1996-12-31'
	) as shipping
group by
	supp_nation,
	cust_nation,
	l_year
order by
	supp_nation,
	cust_nation,
	l_year;

--
--
-- 08.txt
--
--
select 'query 08' as current_query;

-- using 2133894592 as a seed to the RNG
-- set enable_nestloop=off;
explain select
	o_year,
	sum(case
		when nation = 'ROMANIA' then volume
		else 0
	end) / sum(volume) as mkt_share
from
	(
		select
			extract(year from o_orderdate) as o_year,
			l_extendedprice * (1 - l_discount) as volume,
			n2.n_name as nation
		from
			part,
			supplier,
			lineitem,
			orders,
			customer,
			nation n1,
			nation n2,
			region
		where
			p_partkey = l_partkey
			and s_suppkey = l_suppkey
			and l_orderkey = o_orderkey
			and o_custkey = c_custkey
			and c_nationkey = n1.n_nationkey
			and n1.n_regionkey = r_regionkey
			and r_name = 'EUROPE'
			and s_nationkey = n2.n_nationkey
			and o_orderdate between date '1995-01-01' and date '1996-12-31'
			and p_type = 'ECONOMY BRUSHED COPPER'
	) as all_nations
group by
	o_year
order by
	o_year;
--set enable_nestloop=on;

--
--
-- 09.txt
--
--
select 'query 09' as current_query;

-- using 2133894592 as a seed to the RNG
explain select
	nation,
	o_year,
	sum(amount) as sum_profit
from
	(
		select
			n_name as nation,
			extract(year from o_orderdate) as o_year,
			l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity as amount
		from
			part,
			supplier,
			lineitem,
			partsupp,
			orders,
			nation
		where
			s_suppkey = l_suppkey
			and ps_suppkey = l_suppkey
			and ps_partkey = l_partkey
			and p_partkey = l_partkey
			and o_orderkey = l_orderkey
			and s_nationkey = n_nationkey
			and p_name like '%seashell%'
	) as profit
group by
	nation,
	o_year
order by
	nation,
	o_year desc;

--
--
-- 10.txt
--
--
select 'query 10' as current_query;

-- using 2133894592 as a seed to the RNG

explain select
	c_custkey,
	c_name,
	sum(l_extendedprice * (1 - l_discount)) as revenue,
	c_acctbal,
	n_name,
	c_address,
	c_phone,
	c_comment
from
	customer,
	orders,
	lineitem,
	nation
where
	c_custkey = o_custkey
	and l_orderkey = o_orderkey
	and o_orderdate >= date '1993-06-01'
	and o_orderdate < date '1993-06-01' + interval '3 month'
	and l_returnflag = 'R'
	and c_nationkey = n_nationkey
group by
	c_custkey,
	c_name,
	c_acctbal,
	c_phone,
	n_name,
	c_address,
	c_comment
order by
	revenue desc
LIMIT 20;
--
--
-- 11.txt
--
--
select 'query 11' as current_query;

-- using 2133894592 as a seed to the RNG
explain select
	ps_partkey,
	sum(ps_supplycost * ps_availqty) as value
from
	partsupp,
	supplier,
	nation
where
	ps_suppkey = s_suppkey
	and s_nationkey = n_nationkey
	and n_name = 'INDIA'
group by
	ps_partkey having
		sum(ps_supplycost * ps_availqty) > (
			select
				sum(ps_supplycost * ps_availqty) * 0.0000001000
			from
				partsupp,
				supplier,
				nation
			where
				ps_suppkey = s_suppkey
				and s_nationkey = n_nationkey
				and n_name = 'INDIA'
		)
order by
	value desc;

--
--
-- 12.txt
--
--
select 'query 12' as current_query;

-- using 2133894592 as a seed to the RNG
explain select
	l_shipmode,
	sum(case
		when o_orderpriority = '1-URGENT'
			or o_orderpriority = '2-HIGH'
			then 1
		else 0
	end) as high_line_count,
	sum(case
		when o_orderpriority <> '1-URGENT'
			and o_orderpriority <> '2-HIGH'
			then 1
		else 0
	end) as low_line_count
from
	orders,
	lineitem
where
	o_orderkey = l_orderkey
	and l_shipmode in ('AIR', 'MAIL')
	and l_commitdate < l_receiptdate
	and l_shipdate < l_commitdate
	and l_receiptdate >= date '1997-01-01'
	and l_receiptdate < date '1997-01-01' + interval '1 year'
group by
	l_shipmode
order by
	l_shipmode;

--
--
-- 13.txt
--
--
select 'query 13' as current_query;

-- using 2133894592 as a seed to the RNG
explain select
	c_count,
	count(*) as custdist
from
	(
		select
			c_custkey,
			count(o_orderkey)
		from
			customer left outer join orders on
				c_custkey = o_custkey
				and o_comment not like '%unusual%deposits%'
		group by
			c_custkey
	) as c_orders (c_custkey, c_count)
group by
	c_count
order by
	custdist desc,
	c_count desc;

--
--
-- 14.txt
--
--
select 'query 14' as current_query;

-- using 2133894592 as a seed to the RNG
explain select
	100.00 * sum(case
		when p_type like 'PROMO%'
			then l_extendedprice * (1 - l_discount)
		else 0
	end) / sum(l_extendedprice * (1 - l_discount)) as promo_revenue
from
	lineitem,
	part
where
	l_partkey = p_partkey
	and l_shipdate >= date '1997-08-01'
	and l_shipdate < date '1997-08-01' + interval '1 month';

--
--
-- 15.txt
--
--
select 'query 15' as current_query;


-- using 2133894592 as a seed to the RNG


create view revenue0 (supplier_no, total_revenue) as
	select
		l_suppkey,
		sum(l_extendedprice * (1 - l_discount))
	from
		lineitem
	where
		l_shipdate >= date '1997-08-01'
		and l_shipdate < date '1997-08-01' + interval '3 month'
	group by
		l_suppkey;


explain select
	s_suppkey,
	s_name,
	s_address,
	s_phone,
	total_revenue
from
	supplier,
	revenue0
where
	s_suppkey = supplier_no
	and total_revenue = (
		select
			max(total_revenue)
		from
			revenue0
	)
order by
	s_suppkey;

drop view revenue0;

--
--
-- 16.txt
--
--
select 'query 16' as current_query;

-- using 2133894592 as a seed to the RNG
explain select 
	p_brand, 
	p_type, 
	p_size, 
	count(distinct ps_suppkey) as supplier_cnt 
from 
	part, 
	partsupp left join supplier on (ps_suppkey=s_suppkey and s_comment like '%Customer%Complaints%' ) 
where 
	p_partkey = ps_partkey 
	and p_brand <> 'Brand#15' 
	and p_type not like 'LARGE BRUSHED%' 
	and p_size in (4, 41, 17, 9, 15, 2, 25, 34)
	and s_suppkey is null 
group by 
	p_brand, 
	p_type, 
	p_size 
order by 
	supplier_cnt desc, 
	p_brand, 
	p_type, 
	p_size;

--
--
-- 17.txt
--
--
select 'query 17' as current_query;

-- using 2133894592 as a seed to the RNG
explain select
      sum(l_extendedprice) / 7.0 as avg_yearly
from
      lineitem,
      (
          select
              p_partkey as x_partkey,
              0.2 * avg(l_quantity) as x_avg_20
          from
              part,
              lineitem
          where
              p_partkey = l_partkey
              and p_brand = 'Brand#54'
              and p_container = 'SM CAN'
          group by
              p_partkey
      ) x
where
      x_partkey = l_partkey
and l_quantity < x_avg_20;

--
--
-- 18.txt
--
--
select 'query 18' as current_query;

-- using 2133894592 as a seed to the RNG
explain select
	c_name,
	c_custkey,
	o_orderkey,
	o_orderdate,
	o_totalprice,
	sum(l_quantity)
from
	customer,
	orders,
	lineitem
where
	o_orderkey in (
		select
			l_orderkey
		from
			lineitem
		group by
			l_orderkey having
				sum(l_quantity) > 314
	)
	and c_custkey = o_custkey
	and o_orderkey = l_orderkey
group by
	c_name,
	c_custkey,
	o_orderkey,
	o_orderdate,
	o_totalprice
order by
	o_totalprice desc,
	o_orderdate
LIMIT 100;
--
--
-- 19.txt
--
--
select 'query 19' as current_query;

-- using 2133894592 as a seed to the RNG
explain select
	sum(l_extendedprice* (1 - l_discount)) as revenue
from
	lineitem,
	part
where
	(
		p_partkey = l_partkey
		and p_brand = 'Brand#11'
		and p_container in ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
		and l_quantity >= 9 and l_quantity <= 9 + 10
		and p_size between 1 and 5
		and l_shipmode in ('AIR', 'AIR REG')
		and l_shipinstruct = 'DELIVER IN PERSON'
	)
	or
	(
		p_partkey = l_partkey
		and p_brand = 'Brand#51'
		and p_container in ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
		and l_quantity >= 10 and l_quantity <= 10 + 10
		and p_size between 1 and 10
		and l_shipmode in ('AIR', 'AIR REG')
		and l_shipinstruct = 'DELIVER IN PERSON'
	)
	or
	(
		p_partkey = l_partkey
		and p_brand = 'Brand#23'
		and p_container in ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
		and l_quantity >= 22 and l_quantity <= 22 + 10
		and p_size between 1 and 15
		and l_shipmode in ('AIR', 'AIR REG')
		and l_shipinstruct = 'DELIVER IN PERSON'
	);

--
--
-- 20.txt
--
--
select 'query 20' as current_query;

-- using 2133894592 as a seed to the RNG
explain select 
	s_name,
	s_address 
from 
	supplier, 
	nation 
where 
	s_suppkey in( 
		select 
			ps_suppkey 
		from 
			partsupp, 
			( 
				select 
					sum(l_quantity) as qty_sum, l_partkey, l_suppkey 
				from 
					lineitem 
				where 
					l_shipdate >= date '1997-01-01' 
					and l_shipdate < date '1997-01-01' + interval '1 year' 
				group by l_partkey, l_suppkey ) g 
		where 
			g.l_partkey = ps_partkey 
			and g.l_suppkey = ps_suppkey 
			and ps_availqty > 0.5 * g.qty_sum 
			and ps_partkey in ( select p_partkey from part where p_name like 'burnished%' ) 
		) 
	and s_nationkey = n_nationkey 
	and n_name = 'ALGERIA'
order by 
	s_name;


--
--
-- 21.txt
--
--
select 'query 21' as current_query;

-- using 2133894592 as a seed to the RNG
explain select 
	s_name, 
	count(distinct(l1.l_orderkey||l1.l_linenumber)) as numwait 
from 
	supplier, 
	orders, 
	nation, 
	lineitem l1 
		left join lineitem l2 
			on (l2.l_orderkey = l1.l_orderkey and l2.l_suppkey <> l1.l_suppkey) 
		left join (
			select 
				l3.l_orderkey,
				l3.l_suppkey 
			from 
				lineitem l3 
			where 
				l3.l_receiptdate > l3.l_commitdate) l4 
			on (l4.l_orderkey = l1.l_orderkey and l4.l_suppkey <> l1.l_suppkey) 
where 
	s_suppkey = l1.l_suppkey 
	and o_orderkey = l1.l_orderkey 
	and o_orderstatus = 'F' 
	and l1.l_receiptdate > l1.l_commitdate 
	and l2.l_orderkey is not null 
	and l4.l_orderkey is null 
	and s_nationkey = n_nationkey 
	and n_name = 'SAUDI ARABIA' 
group by 
	s_name 
order by 
	numwait desc, 
	s_name
LIMIT 100;

--
--
-- 22.txt
--
--
select 'query 22' as current_query;

-- using 2133894592 as a seed to the RNG
explain select 
	cntrycode, 
	count(*) as numcust, 
	sum(c_acctbal) as totacctbal 
from 
	( 
		select 
			substring(c_phone from 1 for 2) as cntrycode, 
			c_acctbal 
		from 
			customer left join orders 
				on c_custkey = o_custkey 
		where 
			substring(c_phone from 1 for 2) in 
				('10', '28', '11', '15', '18', '16', '19') 
			and c_acctbal > ( 
				select 
					avg(c_acctbal) 
				from 
					customer 
				where 
					c_acctbal > 0.00 
					and substring(c_phone from 1 for 2) in 
						('10', '28', '11', '15', '18', '16', '19') 
			) 
			and o_custkey is null 
	) as custsale 
group by 
	cntrycode 
order by 
	cntrycode;

-- EOF
