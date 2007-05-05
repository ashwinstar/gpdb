SELECT xn, (SELECT COUNT(*) FROM product) FROM util ORDER BY xn;
