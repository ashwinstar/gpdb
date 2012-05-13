-- -----------------------------------------------------------------
-- Test extensions to user-defined function interface (MPP-16349)
-- 	1. VARIADIC arguments
--	2. DEFAULT arguments
--	3. NAMED arguments
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
