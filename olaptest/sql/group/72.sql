select j,k,avg(l),group_id() as gid from test group by rollup(j,k),j order by j,k,gid;
