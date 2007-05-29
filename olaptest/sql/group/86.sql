select cn,vn,grouping(cn,cn,vn)+1 from sale group by rollup(vn,cn);
