select relnatts from pg_class where relname='pg_window';
select distinct relnatts from gp_dist_random('pg_class') where relname='pg_window';

select count(*) from pg_attribute where attrelid='pg_window'::regclass and attnum > 0;
select distinct count(*) from gp_dist_random('pg_attribute') where attrelid='pg_window'::regclass and attnum > 0;

\d pg_window

select distinct pg_window.winframemakerfunc 
  from pg_window, pg_proc 
 where pg_window.winfnoid = pg_proc.oid and (pg_proc.proname='lag' or pg_proc.proname='lead');

select distinct pg_window.winframemakerfunc 
  from pg_window, pg_proc 
 where pg_window.winfnoid = pg_proc.oid and (pg_proc.proname!='lag' and pg_proc.proname!='lead');
