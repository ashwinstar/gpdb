select * from (select row_number() over () from customer) r(a), (select row_number() over () from sale) s(a) where s.a = r.a;
