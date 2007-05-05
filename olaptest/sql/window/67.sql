select row_number() over (), name, loc from (select vname, vloc from vendor union select cname, cloc from customer) r(name,loc);
