select oid, relname, relnamespace, reltype, relowner, relam, relfilenode, reltablespace from gp_dist_random('pg_class') where oid < 10000 order by oid;
