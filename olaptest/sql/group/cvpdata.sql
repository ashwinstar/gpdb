-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
--
-- cvpdata.sql
--
--      Sample data for the schema in cvpschema.sql
--
-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

-- Customers
insert into customer values ( 1, 'Macbeth', 'Inverness');
insert into customer values ( 2, 'Duncan', 'Forres');
insert into customer values ( 3, 'Lady Macbeth', 'Inverness');
insert into customer values ( 4, 'Witches, Inc', 'Lonely Heath');

-- Vendors
insert into vendor values ( 10, 'Witches, Inc', 'Lonely Heath');
insert into vendor values ( 20, 'Lady Macbeth', 'Inverness');
insert into vendor values ( 30, 'Duncan', 'Forres');
insert into vendor values ( 40, 'Macbeth', 'Inverness');
insert into vendor values ( 50, 'Macduff', 'Fife');

-- Products
insert into product values ( 100, 'Sword', 'Black');
insert into product values ( 200, 'Dream', 'Black');
insert into product values ( 300, 'Castle', 'Grey');
insert into product values ( 400, 'Justice', 'Clear');
insert into product values ( 500, 'Donuts', 'Plain');
insert into product values ( 600, 'Donuts', 'Chocolate');
insert into product values ( 700, 'Hamburger', 'Grey');
insert into product values ( 800, 'Fries', 'Grey');


-- Sales (transactions)
insert into sale values ( 2, 40, 100, '1401-1-1', 1100, 2400);
insert into sale values ( 1, 10, 200, '1401-3-1', 1, 0);
insert into sale values ( 3, 40, 200, '1401-4-1', 1, 0);
insert into sale values ( 1, 20, 100, '1401-5-1', 1, 0);
insert into sale values ( 1, 30, 300, '1401-5-2', 1, 0);
insert into sale values ( 1, 50, 400, '1401-6-1', 1, 0);
insert into sale values ( 2, 50, 400, '1401-6-1', 1, 0);
insert into sale values ( 1, 30, 500, '1401-6-1', 12, 5);
insert into sale values ( 3, 30, 500, '1401-6-1', 12, 5);
insert into sale values ( 3, 30, 600, '1401-6-1', 12, 5);
insert into sale values ( 4, 40, 700, '1401-6-1', 1, 1);
insert into sale values ( 4, 40, 800, '1401-6-1', 1, 1);

-- Util

insert into util values (1);
insert into util values (20);
insert into util values (300);

-- Jie's test

drop table test;
drop table t1;
drop table t2;
drop table t3;

create table test (i integer, j integer, k integer, l integer, n integer);
insert into test select i, i%5, i%25, i%50, i%30 from generate_series(0, 100) i;

create table t1(a int, b int, c int, d int) distributed by (a);
create table t2(a int, b int, c int, d int) distributed by (a,b,c);
create table t3(a int, b int, c int, d int) distributed by (a);


insert into t1 values (1,1,1,1),(2,2,2,2),(3,3,3,3),(4,4,4,4);

insert into t3 values (1,1,1,1),(2,null,2,2), (2,2,3,3), (3,3,3,3), (3,null,4,4);

