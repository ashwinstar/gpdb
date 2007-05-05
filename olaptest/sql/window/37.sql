select cn*100 + row_number() over (), cname, row_number() over () -1 from customer;
