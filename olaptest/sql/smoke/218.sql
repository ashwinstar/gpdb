 SELECT cn, (SELECT COUNT(*) FROM product) FROM sale ORDER BY cn;
