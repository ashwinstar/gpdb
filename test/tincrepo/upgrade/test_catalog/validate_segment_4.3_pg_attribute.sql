select * from gp_dist_random('pg_attribute') where attrelid<16000 order by attrelid, attnum;
