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



--
-- Clean up
--

DROP TABLE  gpd1;

