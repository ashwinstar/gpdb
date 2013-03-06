Drop database bkdb;
Create database bkdb;

\c bkdb

CREATE TABLE heap_table (
    column1 integer,
    column2 character varying(20),
    column3 date
) DISTRIBUTED BY (column1);

CREATE TABLE heap_table2 (
    column1 integer,
    column2 character varying(20),
    column3 date
) DISTRIBUTED BY (column1);

insert into heap_table select i, 'backup', i + date '2010-01-01' from generate_series(0,10) as i;
insert into heap_table select i, 'restore', i + date '2010-01-01' from generate_series(0,10) as i;

insert into heap_table2 select i, 'backup', i + date '2010-01-01' from generate_series(0,10) as i;
insert into heap_table2 select i, 'restore', i + date '2010-01-01' from generate_series(0,10) as i;
