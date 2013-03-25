select oid, * from gp_dist_random('pg_namespace') where nspname<>'gp_toolkit' order by oid;
