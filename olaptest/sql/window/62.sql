select cn, row_number() over (partition by cn) from sale;
