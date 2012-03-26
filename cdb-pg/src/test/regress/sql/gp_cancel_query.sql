--
-- Tests for gp_cancel_query 
-- 
-- Mostly negative tests here. Tricky to add positive test cases, as we need to
-- get the session_id, command_id of the running query and input it to 
-- gp_cancel_query to cancel the running query. 
--

-- verify contents of pg_proc for gp_cancel_query
select * from pg_proc where oid = 5075;

drop table if exists test; 

create table test(i int, j int) distributed randomly; 

insert into test values(1, 10); 

-- some negative tests with wrong paramter type for gp_cancel_query
select gp_cancel_query('a', 1) from test; 

select gp_cancel_query(1, 'b'); 

select gp_cancel_query();

-- tests with non-existent session_id and command_id
select gp_cancel_query(-1, 1);
