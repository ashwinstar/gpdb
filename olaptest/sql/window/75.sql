select cn, dt, percent_rank() over (partition by cn order by dt), sum(prc) over (order by cn, dt) from sale;
