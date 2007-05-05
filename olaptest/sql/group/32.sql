select cn+vn as a, vn+pn as b, count(*) from sale group by (1, 2); -- record type issue
