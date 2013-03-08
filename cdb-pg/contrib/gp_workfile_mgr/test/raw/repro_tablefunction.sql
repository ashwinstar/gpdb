
drop table if exists example_etf;
CREATE TABLE example_etf(a int, b text) DISTRIBUTED by (a);
insert into example_etf (select i, 'text' || i from generate_series(0,999999) i); 

-- These functions adapted from installcheck-good / table_functions.source

CREATE OR REPLACE FUNCTION multiset_multi(a anytable) RETURNS TABLE(a int, b text) 
    AS '/Users/caragg/dev/cdb2/private/private-caragg2/cdb-pg/src/test/regress/regress.so', 
    'multiset_example' LANGUAGE C
    IMMUTABLE;

SET gp_workfile_caching=true; 
SET gp_workfile_caching_loglevel="LOG"; 
set statement_mem="5MB";
set gp_resqueue_print_operator_memory_limits=on;

explain analyze SELECT * FROM multiset_multi( TABLE( SELECT * from example_etf) ) order by a, b;
