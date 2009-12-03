-- turn off autostats so we don't have to worry about the logging of the autostat queries
set gp_autostats_mode = None;

-- create needed tables
create table direct_test
(
  key int NULL,
  value varchar(50) NULL
)
distributed by (key); 

create table direct_test_two_column
(
  key1 int NULL,
  key2 int NULL,
  value varchar(50) NULL
)
distributed by (key1, key2);

-- enable printing of printing info
set test_print_direct_dispatch_info=on;

-- Constant single-row insert, one column in distribution
-- DO direct dispatch
insert into direct_test values (100, 'cow');
-- verify
select * from direct_test order by key, value;

-- Constant single-row update, one column in distribution
-- DO direct dispatch
update direct_test set value = 'horse' where key = 100;
-- verify
select * from direct_test order by key, value;

-- Constant single-row delete, one column in distribution
-- DO direct dispatch
delete from direct_test where key = 100;
-- verify
select * from direct_test order by key, value;


-- Constant single-row insert, one column in distribution
-- DO direct dispatch
insert into direct_test values (NULL, 'cow');
-- verify
select * from direct_test order by key, value;

-- Constant single-row insert, two columns in distribution
-- DO direct dispatch
insert into direct_test_two_column values (100, 101, 'cow');
-- verify
select * from direct_test_two_column order by key1, key2, value;

-- Constant single-row update, two columns in distribution
-- DO direct dispatch
update direct_test_two_column set value = 'horse' where key1 = 100 and key2 = 101;
-- verify
select * from direct_test_two_column order by key1, key2, value;

-- Constant single-row delete, two columns in distribution
-- DO direct dispatch
delete from direct_test_two_column where key1 = 100 and key2 = 101;
-- verify
select * from direct_test_two_column order by key1, key2, value;

--------------------------------------------------------------------------------
-- Multiple row update, where clause lists multiple values which hash differently so no direct dispatch
--
-- note that if the hash function for values changes then certain segment configurations may actually 
--                hash all these values to the same content! (and so test would change)
--
update direct_test set value = 'pig' where key in (1,2,3,4,5);

update direct_test_two_column set value = 'pig' where key1 = 100 and key2 in (1,2,3,4);
update direct_test_two_column set value = 'pig' where key1 in (100,101,102,103,104) and key2 in (1);
update direct_test_two_column set value = 'pig' where key1 in (100,101) and key2 in (1,2);

-- Multiple row update, where clause lists values which all hash to same segment
-- DO direct dispatch
-- CAN'T IMPLEMENT THIS TEST BECAUSE THE # of segments changes again (unless we use a # of segments function, and exploit the simple nature of int4 hashing -- can we do that?)


------------------------------
-- Transaction cases
--
-- note that single-row insert can happen BUT DTM will always go to all contents
--
begin;
insert into direct_test values (1,100);
rollback;

begin;
insert into direct_test values (1,100);
insert into direct_test values (2,100);
insert into direct_test values (3,100);
rollback;

----------------------------------------------------------------------------------
----------------------------------------------------------------------------------
----------------------------------------------------------------------------------
-- Prepared statements
--  do same as above ones but using prepared statements, verify data goes to the right spot
-- todo: FIX PREPARE prepare test_insert (int) as insert into direct_test values ($1,100);
-- execute test_insert(1);

-- todo: FIX PREPARE  prepare test_update (int) as update direct_test set value = 'boo' where key = $1;
-- execute test_update(2);

-----------------
-- Cursors
--  do same as above but using cursors


