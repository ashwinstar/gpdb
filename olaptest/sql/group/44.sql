select cn,vn,pn,grouping(cn,vn,pn) from sale group by cn, rollup(vn,pn);
