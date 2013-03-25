select * from pg_description where objoid < 10000 order by objoid, classoid, objsubid;
