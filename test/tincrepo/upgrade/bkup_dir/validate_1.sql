\c bkdb
select * from heap_table;
select * from heap_table2;
insert into heap_table values(12,'newvalue', '2012-12-12');
insert into heap_table2 values(12,'newvalue', '2012-12-12');
select * from heap_table where column1=12;
select * from heap_table2 where column1=12;

