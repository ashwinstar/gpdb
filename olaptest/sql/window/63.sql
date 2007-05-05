select cn, vn, pn, row_number() over (partition by cn), row_number() over (partition by vn), row_number() over (partition by pn) from sale;
