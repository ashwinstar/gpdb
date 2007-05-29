select cn,vn,count(*) from sale group by grouping sets((cn,vn), (cn));
