select cn, vn, pn, avg(qty) over (partition by pn) from sale;
