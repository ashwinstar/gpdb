SELECT * FROM (SELECT * FROM sale) r, (SELECT * FROM util) s WHERE r.cn=s.xn;
