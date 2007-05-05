SELECT avg(qty::int2), avg(qty::int4), avg(qty::int8), avg(qty::numeric), avg(prc::float4), avg(prc::float8), avg('1 month 2 day'::interval) FROM sale;
