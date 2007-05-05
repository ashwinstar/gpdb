select avg(qty) over (partition by cn) from sale;
