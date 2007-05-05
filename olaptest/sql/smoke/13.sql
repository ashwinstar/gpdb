SELECT cname, (SELECT COUNT(*) FROM util) FROM customer ORDER BY cname;
