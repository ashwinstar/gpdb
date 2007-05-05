select cn, dt, ntile(3) over (partition by cn order by dt), sum(prc) over (order by cn, dt) from sale;
