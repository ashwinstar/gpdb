select cn+vn as a, vn+pn as b, count(*) from sale group by (cn+vn), (vn+pn);
