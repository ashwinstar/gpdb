select i,k,avg(l),group_id() as gid from test group by rollup(i,k),i order by i,k,gid;
