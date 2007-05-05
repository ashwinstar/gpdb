select rank() over (order by pn, cn desc), cn, pn from sale;
