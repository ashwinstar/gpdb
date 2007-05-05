select dt, ntile(5) over (order by dt) from sale;
