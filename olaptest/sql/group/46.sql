select cn,vn,pn,grouping(cn,vn,pn) from sale group by vn, rollup(cn, pn);
