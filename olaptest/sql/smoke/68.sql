SELECT * FROM (SELECT * FROM sale) r, (SELECT * FROM sale) s WHERE r.vn=s.cn;
