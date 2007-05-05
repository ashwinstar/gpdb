select cn, vn, pn, avg(qty) over (partition by vn) from sale;
