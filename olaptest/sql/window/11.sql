select row_number() over (partition by pn), cn, pn from sale;
