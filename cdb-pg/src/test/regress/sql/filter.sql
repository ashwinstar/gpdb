DROP TABLE foo;
CREATE TABLE foo (i int, j int) DISTRIBUTED BY (i);
INSERT INTO foo VALUES (1, 1);
INSERT INTO foo VALUES (2, 1);
INSERT INTO foo VALUES (3, 1);
INSERT INTO foo VALUES (4, 2);
INSERT INTO foo VALUES (NULL, 2);
INSERT INTO foo VALUES (6, 2);
INSERT INTO foo VALUES (7, 3);
INSERT INTO foo VALUES (8, NULL);
INSERT INTO foo VALUES (9, 3);
INSERT INTO foo VALUES (10, NULL);

SELECT * FROM foo;

--- TEST COUNT(*)
SELECT count(*) FROM foo;
SELECT count(*) FILTER (WHERE TRUE) FROM foo;
SELECT count(*) FILTER (WHERE FALSE) FROM foo;
SELECT count(*) FILTER (WHERE i < 5) FROM foo;
SELECT count(*) FROM foo WHERE i < 5;
SELECT count(*) FILTER (WHERE j = 1) FROM foo;
SELECT count(*) FROM foo WHERE j = 1;

--- TEST COUNT(i)
SELECT count(i) FROM foo;
SELECT count(i) FILTER (WHERE TRUE) FROM foo;
SELECT count(i) FILTER (WHERE FALSE) FROM foo;
SELECT count(i) FILTER (WHERE i < 5) FROM foo;
SELECT count(i) FROM foo WHERE i < 5;
SELECT count(i) FILTER (WHERE j = 1) FROM foo;
SELECT count(i) FROM foo WHERE j = 1;

--- TEST MAX(*) => should error
SELECT max(*) FROM foo;
SELECT max(*) FILTER (WHERE i < 5) FROM foo;

--- TEST MAX(i)
SELECT max(i) FROM foo;
SELECT max(i) FILTER (WHERE i < 5) FROM foo;
SELECT max(i) FROM foo WHERE i < 5;

--- TEST MIN(i)
SELECT min(i) FROM foo;
SELECT min(i) FILTER (WHERE i < 5) FROM foo;
SELECT min(i) FROM foo WHERE i < 5;

--- TEST MAX(i)
SELECT MAX(i) FROM foo;
SELECT MAX(i) FILTER (WHERE i < 5) FROM foo;
SELECT MAX(i) FROM foo WHERE i < 5;

--- TEST AVG(i)
SELECT AVG(i) FROM foo;
SELECT AVG(i) FILTER (WHERE i < 5) FROM foo;
SELECT AVG(i) FROM foo WHERE i < 5;

--- TEST SUM(i)
SELECT sum(i) FROM foo;
SELECT sum(i) FILTER (WHERE i < 5) FROM foo;
SELECT sum(i) FROM foo WHERE i < 5;


--- TEST with cumulative aggs
SELECT i, j, count(j) OVER (order by i) FROM foo;
SELECT i, j, count(j) FILTER (WHERE i % 2 = 1) OVER (order by i) FROM foo;
SELECT i, j, count(j) OVER (order by i) FROM foo WHERE i % 2 = 1;

--- TEST with partitioned aggs
select i, count(i) over (partition by j) from foo;
select i, count(i) filter (WHERE i % 2 = 1) over (partition by j) from foo;

--- TEST with rolling window aggs
select i, j, count(i) over(w) from foo 
window w as (order by i rows between 1 preceding and 1 following);
select i, j, count(i) filter (where j = 2) over(w) from foo 
window w as (order by i rows between 1 preceding and 1 following);

--- TEST with group by
select j, count(i) from foo group by j;
select o.*, sum(count_num) over (order by j) as count_subtotal
from (select j, count(i) filter (WHERE i%2 = 0) as count_even,
                count(i) filter (WHERE i%2 = 1) as count_odd,
                count(i) as count_num
      from foo group by j) o;
select count(i) from foo;

--- TEST with multi-parameter aggs
select covar_pop(i,j) from foo;
select covar_pop(i,j) from foo where i < 5;
select covar_pop(i,j) filter (where i < 5) from foo;
select covar_pop(i,j) from foo where j in (1,2);
select covar_pop(i,j) filter (where j in (1,2)) from foo;

--- TEST with window functions => should error
select i, row_number() over (partition by j) from foo;
select i, row_number() filter (WHERE i % 2 = 1) over (partition by j) from foo;
select i, rank() over (order by i) from foo;
select i, rank() filter (where true) over (order by i) from foo;
select i, ntile(3) over (order by i) from foo;
select i, ntile(3) filter (where true) over (order by i) from foo;
select i, ntile(4-j) over (partition by j order by i) from foo where j < 4;
select i, ntile(4-j) filter (where true) over (partition by j order by i) from foo where j < 4;

--- TEST against non aggregate function => should error
select i, lower('Hello') from foo;
select i, lower('Hello') filter (where i < 5) from foo;

--- TEST against function like projection => should error
create or replace function maketuple() returns setof foo 
as 'select * from foo' language sql;

select i(t), j(t) from (select maketuple() as t) x;
select i(t) filter (where true) from (select maketuple() as t) x;
drop function maketuple();

--- TEST non-exsistant function => same error
select chocolate(i) from foo;
select chocolate(i) filter (where true) from foo;

--- TEST filter from outside scope => error
select * from (select i from foo where j = 1) x1, 
              (select sum(i) filter (where i < x1.i) from foo where j = 2) x2;

--- TEST against user defined aggregation function
create or replace function _maxodd(oldmax int, newval int) returns int as $$
  SELECT CASE WHEN abs($2) % 2 = 1 AND abs($1) % 2 = 1 
                   THEN (CASE WHEN $1 > $2 THEN $1 ELSE $2 END)
	      WHEN abs($1) % 2 = 1 THEN $1
              WHEN abs($2) % 2 = 1 THEN $2
              ELSE NULL
	      END;
$$ LANGUAGE sql STRICT;

create aggregate maxodd(sfunc = _maxodd, basetype = int, stype = int, initcond = 0);

SELECT maxodd(i) from foo;
SELECT maxodd(i) FILTER (WHERE TRUE) from foo;
SELECT j, maxodd(i) from foo group by j;
SELECT j, maxodd(i) FILTER (WHERE TRUE) from foo group by j;

--- TEST NON-STRICT user defined aggregation function => should error
create or replace function _maxodd(oldmax int, newval int) returns int
as $$
  SELECT CASE WHEN abs($2) % 2 = 1 AND abs($1) % 2 = 1 
                   THEN (CASE WHEN $1 > $2 THEN $1 ELSE $2 END)
	      WHEN abs($1) % 2 = 1 THEN $1
              WHEN abs($2) % 2 = 1 THEN $2
              ELSE NULL
	      END;
$$ LANGUAGE sql;
SELECT maxodd(i) FROM foo;
SELECT maxodd(i) FILTER (WHERE TRUE) from foo;
SELECT j, maxodd(i) FROM foo group by j;
SELECT j, maxodd(i) FILTER (WHERE TRUE) from foo group by j;

drop aggregate maxodd(int);
drop function _maxodd(int,int);
drop table foo;


