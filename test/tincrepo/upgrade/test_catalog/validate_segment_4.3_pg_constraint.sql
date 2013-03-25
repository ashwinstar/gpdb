select oid, * from gp_dist_random('pg_constraint') order by oid;
