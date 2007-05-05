SELECT cname, (SELECT COUNT(*) FROM product) FROM customer ORDER BY cname;
