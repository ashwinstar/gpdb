select row_number() over (), s from generate_series(1,3) r(s);
