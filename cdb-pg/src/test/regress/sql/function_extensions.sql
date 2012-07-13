-- -----------------------------------------------------------------
-- Test extensions to user-defined function interface (MPP-16349)
-- 	1. VARIADIC arguments
--	2. DEFAULT arguments
--	3. NAMED arguments
--
-- Test additional extensions (MPP-16060)
-- 	1. data_access indicators
-- -----------------------------------------------------------------

-- -----------------------------------------------------------------
-- 	1. VARIADIC arguments
-- -----------------------------------------------------------------
-- POLYMORPHISM
-- test variadic polymorphic functions
/*
create function myleast(variadic anyarray) returns anyelement as $$
  select min($1[i]) from generate_subscripts($1,1) g(i)
$$ language sql immutable strict;

select myleast(10, 1, 20, 33);
select myleast(1.1, 0.22, 0.55);
select myleast('z'::text);
select myleast(); -- fail

-- test with variadic call parameter
select myleast(variadic array[1,2,3,4,-1]);
select myleast(variadic array[1.1, -5.5]);

--test with empty variadic call parameter
select myleast(variadic array[]::int[]);
*/

-- an example with some ordinary arguments too
create function concat(text, variadic anyarray) returns text as $$
  select array_to_string($2, $1);
$$ language sql immutable strict;

select concat('%', 1, 2, 3, 4, 5);
select concat('|', 'a'::text, 'b', 'c');
select concat('|', variadic array[1,2,33]);
select concat('-|-', variadic '{44,NULL,55,NULL,33}'::int[]);
-- select concat('|', variadic array[]::int[]); -- MPP-11851

-- deparse view
create table people (id int, fname varchar, lname varchar);
insert into people values (770,'John','Woo');
insert into people values (771,'Jim','Ng');
insert into people values (778,'Jerry','Lau');
insert into people values (790,'Jen','Smith');
create view print_name as select concat(' ',id, fname, lname) from people; -- fail, type mismatch
create view print_name as select concat(' ', fname, lname) from people where id < 790;

select pg_get_viewdef('print_name');
select pg_get_viewdef('print_name', true);

drop view print_name;
drop function concat(text, anyarray);

-- mix variadic with anyelement
create function formarray(anyelement, variadic anyarray) returns anyarray as $$
  select array_prepend($1, $2);
$$ language sql immutable strict;

select formarray(1,2,3,4,5);
select formarray(1.1, variadic array[1.2,55.5]);
select formarray(1.1, array[1.2,55.5]); -- fail without variadic
select formarray(1, 'x'::text); -- fail, type mismatch
select formarray(1, variadic array['x'::text]); -- fail, type mismatch

drop function formarray(anyelement, variadic anyarray);

-- PLPGSQL
-- test variadic functions

create or replace function vari(variadic int[])
returns void as $$
begin
  for i in array_lower($1,1)..array_upper($1,1) loop
    raise notice '%', $1[i];
  end loop; end;
$$ language plpgsql;

select vari(1,2,3,4,5);
select vari(3,4,5);
select vari(variadic array[5,6,7]);

drop function vari(int[]);

-- coercion test
create or replace function pleast(variadic numeric[])
returns numeric as $$
declare aux numeric = $1[array_lower($1,1)];
begin
  for i in array_lower($1,1)+1..array_upper($1,1) loop
    if $1[i] < aux then aux := $1[i]; end if;
  end loop;
  return aux;
end;
$$ language plpgsql immutable strict;

select pleast(10,1,2,3,-16);
select pleast(10.2,2.2,-1.1);
select pleast(10.2,10, -20);
select pleast(10,20, -1.0);

-- in case of conflict, non-variadic version is preferred
create or replace function pleast(numeric)
returns numeric as $$
begin
  raise notice 'non-variadic function called';
  return $1;
end;
$$ language plpgsql immutable strict;

select pleast(10);
select pleast(1,-2,3.3);

drop function pleast(numeric[]);
drop function pleast(numeric);


-- table function
create function tfunc(variadic char[]) returns table (id int, tx char) as 
$$ select id, unnest($1) || ', ' ||  lname || '.' || fname from people order by 2
$$ language sql strict;

select * from tfunc ('hello', 'morning');

drop table people;
drop function tfunc(variadic char[]);


-- -----------------------------------------------------------------
--	2. DEFAULT arguments
-- -----------------------------------------------------------------
-- test functions with parameter defaults
-- test basic functionality
create function dfunc(a int = 1, int = 2) returns int as $$ select $1 + $2; $$ language sql;

select dfunc();
select dfunc(10);
select dfunc(10, 20);
select dfunc(10, 20, 30);  -- fail

drop function dfunc();  -- fail
drop function dfunc(int);  -- fail
drop function dfunc(int, int);  -- ok

-- fail: defaults must be at end of argument list
create function dfunc(a int = 1, b int) returns int as $$ select $1 + $2; $$ language sql;

-- however, this should work:
create function dfunc(a int = 1, out sum int, b int = 2) as $$ select $1 + $2; $$ language sql;
select dfunc();

-- verify it lists properly
\df dfunc

-- inout argmode test:
drop function dfunc(int, int);
create function dfunc(a inout int default 10, b int = 2) as $$ select $1 + $2; $$ language sql;

select dfunc();
select dfunc(4);
select dfunc(4,5);
\df dfunc

drop function dfunc(int, int);

-- check implicit coercion 
create function dfunc(a int DEFAULT 1.0, int DEFAULT '-1') returns int as $$ select $1 + $2; $$ language sql;
select dfunc();

create function dfunc(a text DEFAULT 'Hello', b text DEFAULT 'World') returns text as $$ select $1 || ', ' || $2; $$ language sql;

select dfunc();  -- fail: which dfunc should be called? int or text
select dfunc('Hi');  -- ok
select dfunc('Hi', 'City');  -- ok
select dfunc(0);  -- ok
select dfunc(10, 20);  -- ok

drop function dfunc(int, int);
drop function dfunc(text, text);


-- ambiguity should be reported only if there's not a better match available
create function dfunc(text) returns text as $$ select $1; $$ language sql;
create function dfunc(int = 1, int = 2) returns int as $$ select 2; $$ language sql;
create function dfunc(int = 1, int = 2, int = 3, int = 4) returns int as $$ select 4; $$ language sql;

-- now, dfunc(nargs = 2) and dfunc(nargs = 4) are ambiguous when called
-- with 0 to 2 arguments with input type int

select dfunc('Hi');	-- ok, input type text
select dfunc();  -- fail
select dfunc(1);  -- fail
select dfunc(1, 2);  -- fail
select dfunc(1, 2, 3);  -- ok
select dfunc(1, 2, 3, 4);  -- ok

drop function dfunc(text);
drop function dfunc(int, int);
drop function dfunc(int, int, int, int);

-- default values are not allowed for output parameters
create function dfunc(out int = 20) returns int as $$ select 1; $$ language sql;

-- polymorphic parameter test
create function dfunc(anyelement = 'World'::text) returns text as $$ select 'Hello, ' || $1::text; $$ language sql;
select dfunc();
select dfunc(0);
select dfunc(to_date('20081215','YYYYMMDD'));
select dfunc('City'::text);

drop function dfunc(anyelement);

-- check defaults for variadics
create function dfunc(a variadic int[]) returns int as
$$ select array_upper($1, 1) $$ language sql;

select dfunc();  -- fail
select dfunc(10);
select dfunc(10,20);

create or replace function dfunc(a variadic int[] default '{44,88,99}'::int[]) returns int as $$ select array_upper($1, 1) $$ language sql;

select dfunc();  -- now ok
select dfunc(10);
select dfunc(10,20);

-- can change default value
create or replace function dfunc(a variadic int[] default '{44,55,88,99}'::int[]) returns int as $$ select array_upper($1, 1) $$ language sql;
select dfunc();

-- can't remove the default once it exists
create or replace function dfunc(a variadic int[]) returns int as $$ select array_upper($1, 1) $$ language sql;
\df dfunc

drop function dfunc(a variadic int[]);

-- test expression
create function dfunc(a int, b double precision default random()*10) returns double precision as $$ select $1+$2; $$ language sql;
\df dfunc

create view dview as select random()*10, dfunc(4);
select pg_get_viewdef('dview',true);

-- need drop cascade
drop function dfunc(int, double precision);
drop function dfunc(int, double precision) cascade;


-- -----------------------------------------------------------------
--	3. NAMED arguments
-- -----------------------------------------------------------------

--
-- Tests for named- and mixed-notation function calling
--

create function dfunc(a int, b int, c int = 0, d int = 0)
  returns table (a int, b int, c int, d int) as $$
  select $1, $2, $3, $4;
$$ language sql;

select (dfunc(10,20,30)).*;
select (dfunc(a => 10, b => 20, c => 30)).*;
select * from dfunc(a => 10, b => 20);
select * from dfunc(b => 10, a => 20);
select * from dfunc(0);  -- fail
select * from dfunc(1,2);
select * from dfunc(1,2, c => 3);
select * from dfunc(1,2, d => 3);

select * from dfunc(x => 10, b => 20, x => 30);  -- fail, duplicate name
select * from dfunc(10, b => 20, 30);  -- fail, named args must be last
select * from dfunc(x => 10, b => 20, c => 30);  -- fail, unknown param
select * from dfunc(10, 10, a => 20);  -- fail, a overlaps positional parameter
select * from dfunc(1, c => 2, d => 3); -- fail, no value for b

drop function dfunc(int, int, int, int);

-- test with different parameter types
create function dfunc(a varchar, b numeric, c date = current_date)
  returns table (a varchar, b numeric, c date) as $$
  select $1, $2, $3;
$$ language sql;

select (dfunc('Hello World', 20, '2009-07-25'::date)).*;
select * from dfunc('Hello World', 20, '2009-07-25'::date);
select * from dfunc(c => '2009-07-25'::date, a => 'Hello World', b => 20);
select * from dfunc('Hello World', b => 20, c => '2009-07-25'::date);
select * from dfunc('Hello World', c => '2009-07-25'::date, b => 20);
select * from dfunc('Hello World', c => 20, b => '2009-07-25'::date);  -- fail

drop function dfunc(varchar, numeric, date);

-- test out parameters with named params
create function dfunc(a varchar = 'def a', out _a varchar, c numeric = NULL, out _c numeric)
returns record as $$
  select $1, $2;
$$ language sql;

select (dfunc()).*;
select * from dfunc();
select * from dfunc('Hello', 100);
select * from dfunc(a => 'Hello', c => 100);
select * from dfunc(c => 100, a => 'Hello');
select * from dfunc('Hello');
select * from dfunc('Hello', c => 100);
select * from dfunc(c => 100);

-- fail, can no longer change an input parameter's name
create or replace function dfunc(a varchar = 'def a', out _a varchar, x numeric = NULL, out _c numeric)
returns record as $$
  select $1, $2;
$$ language sql;

create or replace function dfunc(a varchar = 'def a', out _a varchar, numeric = NULL, out _c numeric)
returns record as $$
  select $1, $2;
$$ language sql;

drop function dfunc(varchar, numeric);

--fail, named parameters are not unique
create function testfoo(a int, a int) returns int as $$ select 1;$$ language sql;
create function testfoo(int, out a int, out a int) returns int as $$ select 1;$$ language sql;
create function testfoo(out a int, inout a int) returns int as $$ select 1;$$ language sql;
create function testfoo(a int, inout a int) returns int as $$ select 1;$$ language sql;

-- valid
create function testfoo(a int, out a int) returns int as $$ select $1;$$ language sql;
select testfoo(37);
drop function testfoo(int);
create function testfoo(a int) returns table(a int) as $$ select $1;$$ language sql;
select * from testfoo(37);
drop function testfoo(int);

-- test polymorphic params and defaults
create function dfunc(a anyelement, b anyelement = null, flag bool = true)
returns anyelement as $$
  select case when $3 then $1 else $2 end;
$$ language sql;

select dfunc(1,2);
select dfunc('a'::text, 'b'); -- positional notation with default

select dfunc(a => 1, b => 2);
select dfunc(a => 'a'::text, b => 'b');
select dfunc(a => 'a'::text, b => 'b', flag => false); -- named notation

select dfunc(b => 'b'::text, a => 'a'); -- named notation with default
select dfunc(a => 'a'::text, flag => true); -- named notation with default
select dfunc(a => 'a'::text, flag => false); -- named notation with default
select dfunc(b => 'b'::text, a => 'a', flag => true); -- named notation

select dfunc('a'::text, 'b', false); -- full positional notation
select dfunc('a'::text, 'b', flag => false); -- mixed notation
select dfunc('a'::text, 'b', true); -- full positional notation
select dfunc('a'::text, 'b', flag => true); -- mixed notation

-- check reverse-listing of named-arg calls
CREATE VIEW dfview AS
   SELECT q1, q2,
     dfunc(q1,q2, flag => q1>q2) as c3,
     dfunc(q1, flag => q1<q2, b => q2) as c4
     FROM int8_tbl;

select * from dfview;

\d dfview

drop view dfview;
drop function dfunc(anyelement, anyelement, bool);

-- test prodataaccess 
create function func1(int, int) returns int as
$$
  select $1 + $2; 
$$ language sql immutable contains sql;

-- check prodataaccess column in pg_proc 
select proname, prodataaccess from pg_proc where proname = 'func1';

create function func2(a anyelement, b anyelement = null, flag bool = true) 
returns anyelement as
$$
  select $1 + $2; 
$$ language sql reads sql data; 

-- check prodataaccess column in pg_proc 
select proname, prodataaccess from pg_proc where proname = 'func2';

create function func3() returns oid as
$$
  select oid from pg_class where relname = 'pg_type'; 
$$ language sql modifies sql data volatile;

-- check prodataaccess column in pg_proc 
select proname, prodataaccess from pg_proc where proname = 'func3';

-- check default value of prodataaccess
drop function func1(int, int);
create function func1(int, int) returns varchar as $$
declare
	v_name varchar(20) DEFAULT 'zzzzz';
begin
	select relname from pg_class into v_name where oid=$1;
	return v_name;
end;
$$ language plpgsql;

select proname, proargnames, prodataaccess from pg_proc where proname = 'func1';

create function func4(int, int) returns int as
$$
  select $1 + $2; 
$$ language sql;

-- check prodataaccess column
select proname, proargnames, prodataaccess from pg_proc where proname = 'func4';

-- change prodataaccess option
create or replace function func4(int, int) returns int as
$$
  select $1 + $2; 
$$ language sql modifies sql data;

select proname, proargnames, prodataaccess from pg_proc where proname = 'func4';

-- data_access indicators for plpgsql
drop function func1(int, int);
create or replace function func1(int, int) returns varchar as $$
declare
	v_name varchar(20) DEFAULT 'zzzzz';
begin
	select relname from pg_class into v_name where oid=$1;
	return v_name;
end;
$$ language plpgsql reads sql data;

select proname, proargnames, prodataaccess from pg_proc where proname = 'func1';

-- check conflicts
drop function func1(int, int);
create function func1(int, int) returns int as
$$
  select $1 + $2; 
$$ language sql immutable no sql;

create function func1(int, int) returns int as
$$
  select $1 + $2; 
$$ language sql immutable reads sql data;

drop function func2(anyelement, anyelement, bool);
drop function func3();
drop function func4(int, int);
