--
-- CREATE_AGGREGATE
--

-- all functions CREATEd
CREATE AGGREGATE newavg (
   sfunc = int4_avg_accum, basetype = int4, stype = bytea, 
   finalfunc = int8_avg,
   initcond1 = '{0}'
);

-- test comments
COMMENT ON AGGREGATE newavg_wrong (int4) IS 'an agg comment';
COMMENT ON AGGREGATE newavg (int4) IS 'an agg comment';
COMMENT ON AGGREGATE newavg (int4) IS NULL;

-- without finalfunc; test obsolete spellings 'sfunc1' etc
CREATE AGGREGATE newsum (
   sfunc1 = int4pl, basetype = int4, stype1 = int4, 
   initcond1 = '0'
);

-- zero-argument aggregate
CREATE AGGREGATE newcnt (*) (
   sfunc = int8inc, stype = int8,
   initcond = '0'
);

-- old-style spelling of same
CREATE AGGREGATE oldcnt (
   sfunc = int8inc, basetype = 'ANY', stype = int8,
   initcond = '0'
);

-- aggregate that only cares about null/nonnull input
CREATE AGGREGATE newcnt ("any") (
   sfunc = int8inc_any, stype = int8,
   initcond = '0'
);

-- multi-argument aggregate
create function sum3(int8,int8,int8) returns int8 as
'select $1 + $2 + $3' language sql strict immutable;

create aggregate sum2(int8,int8) (
   sfunc = sum3, stype = int8,
   initcond = '0'
);


-- multi-argument aggregates sensitive to distinct/order, strict/nonstrict

/*
-- MPP: In Postgres this creates an array type with it, in GP it does not.
create type aggtype as (a integer, b integer, c text);

create function aggf_trans(aggtype[],integer,integer,text) returns aggtype[]
as 'select array_append($1,ROW($2,$3,$4)::aggtype)'
language sql strict immutable;

create function aggfns_trans(aggtype[],integer,integer,text) returns aggtype[]
as 'select array_append($1,ROW($2,$3,$4)::aggtype)'
language sql immutable;

create aggregate aggfstr(integer,integer,text) (
   sfunc = aggf_trans, stype = aggtype[],
   initcond = '{}'
);

create aggregate aggfns(integer,integer,text) (
   sfunc = aggfns_trans, stype = aggtype[],
   initcond = '{}'
);
*/
create function aggf_trans(text[],integer,integer,text) returns text[]
as 'select array_append($1, textin(record_out(ROW($2,$3,$4))))'
language sql strict immutable;

create function aggfns_trans(text[],integer,integer,text) returns text[]
as 'select array_append($1, textin(record_out(ROW($2,$3,$4))))'
language sql immutable;

create ordered aggregate aggfstr(integer,integer,text) (
   stype = text[],
   sfunc = aggf_trans, 
   initcond = '{}'
);

create ordered aggregate aggfns(integer,integer,text) (
   stype = text[],
   sfunc = aggfns_trans, 
   initcond = '{}'
);

-- Negative test: "ordered aggregate prefunc is not supported"
create ordered aggregate should_error(integer,integer,text) (
   stype = text[],
   sfunc = aggfns_trans, 
   prefunc = array_cat,
   initcond = '{}'
);

-- Test cases around prelimary and inverse functions
create aggregate my_average1(bigint) (
   stype      = bytea,
   initcond   = '',
   sfunc      = int8_avg_accum,
   invsfunc   = int8_avg_decum,
   prefunc    = int8_avg_amalg,
   invprefunc = int8_avg_demalg,
   finalfunc  = int8_avg
);

-- ERROR: an sfunc is manditory
create aggregate should_error(bigint) (
   stype      = bytea,
   initcond   = '',
--   sfunc      = int8_avg_accum,
   invsfunc   = int8_avg_decum,
   prefunc    = int8_avg_amalg,
   invprefunc = int8_avg_demalg,
   finalfunc  = int8_avg
);
DROP AGGREGATE IF EXISTS should_error(bigint);

-- ERROR: need prefunc to support invprefunc
create aggregate should_error(bigint) (
   stype      = bytea,
   initcond   = '',
   sfunc      = int8_avg_accum,
   invsfunc   = int8_avg_decum,
--   prefunc    = int8_avg_amalg,
   invprefunc = int8_avg_demalg,
   finalfunc  = int8_avg
);
DROP AGGREGATE IF EXISTS should_error(bigint);

create aggregate my_average2(bigint) (
   stype      = bytea,
   initcond   = '',
   sfunc      = int8_avg_accum,
   invsfunc   = int8_avg_decum,
   prefunc    = int8_avg_amalg,
--   invprefunc = int8_avg_demalg,
   finalfunc  = int8_avg
);

create aggregate my_average3(bigint) (
   stype      = bytea,
   initcond   = '',
   sfunc      = int8_avg_accum,
--   invsfunc   = int8_avg_decum,
   prefunc    = int8_avg_amalg,
   invprefunc = int8_avg_demalg,
   finalfunc  = int8_avg
); 

create aggregate my_average4(bigint) (
   stype      = bytea,
   initcond   = '',
   sfunc      = int8_avg_accum,
--   invsfunc   = int8_avg_decum,
   prefunc    = int8_avg_amalg,
--   invprefunc = int8_avg_demalg,
   finalfunc  = int8_avg
); 



-- All 4 valid combinations should produce identical results
create table aggtest1 as select i::bigint from generate_series(1,10) i;
--begin_equivalent
SELECT avg(i) from aggtest1;
SELECT avg(i) from generate_series(1,10) i;
select my_average1(i) from aggtest1;
SELECT my_average1(i) from generate_series(1,10) i;
select my_average2(i) from aggtest1;
SELECT my_average2(i) from generate_series(1,10) i;
select my_average3(i) from aggtest1;
SELECT my_average3(i) from generate_series(1,10) i;
select my_average4(i) from aggtest1;
SELECT my_average4(i) from generate_series(1,10) i;
--end_equivalent

--begin_equivalent
SELECT i%2, avg(i) from aggtest1 group by rollup (i%2);
SELECT i%2, avg(i) from generate_series(1,10) i group by rollup (i%2);
select i%2, my_average1(i) from aggtest1 group by rollup (i%2);
SELECT i%2, my_average1(i) from generate_series(1,10) i group by rollup (i%2);
select i%2, my_average2(i) from aggtest1 group by rollup (i%2);
SELECT i%2, my_average2(i) from generate_series(1,10) i group by rollup (i%2);
select i%2, my_average3(i) from aggtest1 group by rollup (i%2);
SELECT i%2, my_average3(i) from generate_series(1,10) i group by rollup (i%2);
select i%2, my_average4(i) from aggtest1 group by rollup (i%2);
SELECT i%2, my_average4(i) from generate_series(1,10) i group by rollup (i%2);
--end_equivalent

--begin_equivalent
SELECT avg(i) over (w) from aggtest1 window w as (order by i rows between 2 preceding and 2 following);
SELECT avg(i) over (w) from generate_series(1,10) i window w as (order by i rows between 2 preceding and 2 following);
select my_average1(i) over (w) from aggtest1 window w as (order by i rows between 2 preceding and 2 following);
SELECT my_average1(i) over (w) from generate_series(1,10) i window w as (order by i rows between 2 preceding and 2 following);
select my_average2(i) over (w) from aggtest1 window w as (order by i rows between 2 preceding and 2 following);
SELECT my_average2(i) over (w) from generate_series(1,10) i window w as (order by i rows between 2 preceding and 2 following);
select my_average3(i) over (w) from aggtest1 window w as (order by i rows between 2 preceding and 2 following);
SELECT my_average3(i) over (w) from generate_series(1,10) i window w as (order by i rows between 2 preceding and 2 following);
select my_average4(i) over (w) from aggtest1 window w as (order by i rows between 2 preceding and 2 following);
SELECT my_average4(i) over (w) from generate_series(1,10) i window w as (order by i rows between 2 preceding and 2 following);
--end_equivalent

SELECT * FROM pg_aggregate where aggfnoid in (
        'avg(bigint)'::regprocedure::regproc,
        'my_average1'::regproc,
        'my_average2'::regproc,
        'my_average3'::regproc,
        'my_average4'::regproc
  );

DROP AGGREGATE IF EXISTS my_average1(bigint);
DROP AGGREGATE IF EXISTS my_average2(bigint);
DROP AGGREGATE IF EXISTS my_average3(bigint);
DROP AGGREGATE IF EXISTS my_average4(bigint);
DROP TABLE IF EXISTS aggtest1;

-- Comments on aggregates
COMMENT ON AGGREGATE nosuchagg (*) IS 'should fail';
COMMENT ON AGGREGATE newcnt (*) IS 'an agg(*) comment';
COMMENT ON AGGREGATE newcnt ("any") IS 'an agg(any) comment';

-- MPP-2863: ensure that aggregate declarations with an initial value == ''
-- do not get converted to an initial value == NULL
create function str_concat(t1 text, t2 text) returns text as
$$
    select $1 || $2;
$$ language sql;

CREATE AGGREGATE string_concat (sfunc = str_concat, prefunc=str_concat, basetype = 'text', stype = text,initcond = '');

create table aggtest2(i int, t text) DISTRIBUTED BY (i);
insert into aggtest2 values(1, 'hello');
insert into aggtest2 values(2, 'hello');
select string_concat(t) from aggtest2;
select string_concat(t) from (select * from aggtest2 limit 2000) tmp;
drop table aggtest2;
drop aggregate string_concat(text);
drop function str_concat(text, text);
