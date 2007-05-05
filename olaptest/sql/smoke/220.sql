SELECT cname, (SELECT COUNT(*) FROM sale) FROM customer ORDER BY cname;
