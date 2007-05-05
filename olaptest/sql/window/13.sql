select row_number() over (partition by pn, cn), cn, pn from sale;
