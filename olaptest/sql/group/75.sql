select i,k,group_id() as gid from test group by rollup(i,k),i order by i,k,gid;
