-- start_ignore
alter system set autovacuum = off;
select * from pg_reload_conf();
-- end_ignore
