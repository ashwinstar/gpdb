select row_number() over (order by pn, cn desc), cn, pn from sale;
