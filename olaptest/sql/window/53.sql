select cn,vn, rank() over (order by cn), rank() over (order by cn,vn) from sale;
