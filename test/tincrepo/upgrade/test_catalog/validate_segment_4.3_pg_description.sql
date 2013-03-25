select * from gp_dist_random('pg_description') where objoid < 10000 order by objoid, classoid, objsubid;
