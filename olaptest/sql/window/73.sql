select cn, dt, ntile(3) over (partition by cn order by dt) from sale;
