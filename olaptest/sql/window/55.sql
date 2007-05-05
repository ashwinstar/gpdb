select cn,vn, row_number() over (partition by cn,vn), cn,vn, rank() over (partition by cn,vn order by vn), cn,vn,pn, rank() over (partition by cn,vn order by vn,pn) from sale;
