 SELECT * FROM util WHERE xn in (SELECT xn FROM util) ORDER BY xn;
