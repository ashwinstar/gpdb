-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
--
-- qadbhead.sql
--
--     Set test aids, schema, and data in CDB HEAD node. 
--
-- See cdbREADME.txt for more information.
--
-- $Id$
--
-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

\pset tuples_only

--
-- Record the star schema for CDB
--

SELECT cdb.partition_table('customer', '{1}');
SELECT cdb.partition_table('vendor', '{1}');
SELECT cdb.partition_table('product', '{1}');
SELECT cdb.partition_table('sale', '{1,2,3}');
SELECT cdb.partition_table('util', '{1}');
select cdb.partition_table('leagues', '{1}');
select cdb.partition_table('divisions', '{1}');
select cdb.partition_table('teams', '{1}');
select cdb.partition_table('positions', '{1}');
select cdb.partition_table('races', '{1}');
select cdb.partition_table('drugs', '{1}');
select cdb.partition_table('skills', '{1}');
select cdb.partition_table('pregame_meals', '{1}');
select cdb.partition_table('superstitions', '{1}');
select cdb.partition_table('mlb_player_fact', '{1}');
select cdb.partition_table('Max_Cols', '{1}');
select cdb.partition_table('dtype_text', '{1}');
select cdb.partition_table('dtype_varchar', '{1}');
select cdb.partition_table('dtype_char', '{1}');
select cdb.partition_table('dtype_int', '{1}');
select cdb.partition_table('dtype_int2', '{1}');
select cdb.partition_table('dtype_int8', '{1}');
select cdb.partition_table('dtype_numeric', '{1}');
select cdb.partition_table('dtype_float', '{1}');
select cdb.partition_table('dtype_float4', '{1}');
select cdb.partition_table('dtype_date', '{1}');
select cdb.partition_table('dtype_time', '{1}');
select cdb.partition_table('dtype_timestamp', '{1}');
select cdb.partition_table('dtype_interval', '{1}');
select cdb.partition_table('dtype_boolean', '{1}');
select cdb.partition_table('all_dtypes', '{1}');
