select oid, * from gp_dist_random('pg_proc') where pronamespace<>(select oid from pg_namespace where nspname='gp_toolkit') order by oid; -- Exclude gp_toolkit
