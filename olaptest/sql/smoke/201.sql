SELECT * FROM util WHERE xn in (SELECT vn FROM sale) ORDER BY xn;
