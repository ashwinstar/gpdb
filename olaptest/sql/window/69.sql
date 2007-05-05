select avg(qty) over (partition by vn) from sale;
