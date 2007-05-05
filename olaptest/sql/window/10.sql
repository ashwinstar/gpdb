select row_number() over (partition by cn), cn, pn from sale;
