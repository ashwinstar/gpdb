select cn,vn,pn,grouping(cn,vn,pn) from sale group by rollup(cn),vn,pn;
