select j,k,group_id() as gid from test group by rollup(j,k),j order by j,k,gid;
