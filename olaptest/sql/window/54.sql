select cn,vn,pn, row_number() over (partition by cn), rank() over (partition by cn order by vn), rank() over (partition by cn order by vn,pn) from sale;
