select cn, vn, pn, avg(qty) over (partition by cn) from sale;
