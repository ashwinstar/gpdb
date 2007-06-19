--
-- BOOLEAN
--

create table gpd1 (c1 char(1), c2 numeric, c3 numeric);
insert into gpd1 values ('a', 1, 1);
insert into gpd1 values ('a', 1, 2);
insert into gpd1 values ('b', 2, 1);
insert into gpd1 values ('b', 1, 2);
insert into gpd1 values ('c', 3, 2);
insert into gpd1 values ('c', 2, 3);
insert into gpd1 values ('d', 4, 4);
insert into gpd1 values ('d', 4, 3);
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
--
-- Clean up
--

DROP TABLE  gpd1;

