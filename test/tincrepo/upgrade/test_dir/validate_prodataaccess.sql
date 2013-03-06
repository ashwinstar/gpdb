select relnatts from pg_class where relname='pg_proc';
select distinct relnatts from gp_dist_random('pg_class') where relname='pg_proc';

select count(*) from pg_attribute where attrelid='pg_proc'::regclass and attnum > 0;
select distinct count(*) from gp_dist_random('pg_attribute') where attrelid='pg_proc'::regclass and attnum > 0;

\d pg_proc

-- MODIFIES SQL
select proname, prodataaccess from pg_proc
where oid = 1645;

-- READS SQL
select proname, prodataaccess from pg_proc
where oid = 2324; 

-- READS SQL
select proname, prodataaccess from pg_proc
where oid = 1371 or oid = 2168 or oid = 6023;

-- READS SQL
select proname, prodataaccess from pg_proc
where oid = 2288; 

-- READS SQL
select proname, prodataaccess from pg_proc 
where oid = 7169; 

-- MODIFIES SQL
select proname, prodataaccess from pg_proc 
where oid >= 7173 and oid <= 7174;

-- CONTAINS SQL 
select proname, prodataaccess from pg_proc 
where prolang = 14;

-- NO SQL
select proname, prodataaccess from pg_proc 
where oid = 1242;

select * from pg_proc
where proname = 'func1';

select * from pg_proc
where proname = 'func2';
