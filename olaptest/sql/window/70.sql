select avg(qty) over (partition by pn) from sale;
