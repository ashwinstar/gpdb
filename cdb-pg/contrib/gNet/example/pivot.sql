create table pivot_example 
(id int, 
 attr text, 
 attr_int2 int2,
 attr_int4 int4,
 attr_int8 int8,
 attr_float4 float4,
 attr_float8 float8,
 attr_numeric numeric
)
distributed by (id);
insert into pivot_example values (1, 'GENDER', 0,0,0,0,0,0);
insert into pivot_example values (1, 'AGE', 32,32,32,32,32,32);
insert into pivot_example values (1, 'HEIGHT', 68,68,68,68,68,68);
insert into pivot_example values (1, 'WEIGHT', 132,132,132,132,132,132);
insert into pivot_example values (2, 'GENDER', 1,1,1,1,1,1);
insert into pivot_example values (2, 'AGE', 29,29,29,29,29,29);
insert into pivot_example values (2, 'HEIGHT', 64,64,64,64,64,64);
insert into pivot_example values (2, 'WEIGHT', 112,112,112,112,112,112);
insert into pivot_example values (3, 'GENDER', 0,0,0,0,0,0);
insert into pivot_example values (3, 'AGE', 36,36,36,36,36,36);
insert into pivot_example values (3, 'HEIGHT', 69,69,69,69,69,69);
insert into pivot_example values (3, 'WEIGHT', 164,164,164,164,164,164);
insert into pivot_example values (NULL, 'GENDER', 1,1,1,1,1,1);
insert into pivot_example values (4, 'HEIGHT');
insert into pivot_example values (4, 'WEIGHT', 164,164,164,164,164,164);


select * from pivot_example;

select id, pivot_sum(ARRAY['GENDER', 'AGE', 'HEIGHT', 'WEIGHT'], attr, attr_int2)
from pivot_example
group by id;

select id, pivot_sum(ARRAY['GENDER', 'AGE', 'HEIGHT', 'WEIGHT'], attr, attr_int4)
from pivot_example
group by id;

select id, pivot_sum(ARRAY['GENDER', 'AGE', 'HEIGHT', 'WEIGHT'], attr, attr_int8)
from pivot_example
group by id;

select id, pivot_sum(ARRAY['GENDER', 'AGE', 'HEIGHT', 'WEIGHT'], attr, attr_float4)
from pivot_example
group by id;

select id, pivot_sum(ARRAY['GENDER', 'AGE', 'HEIGHT', 'WEIGHT'], attr, attr_float8)
from pivot_example
group by id;

select id, pivot_sum(ARRAY['GENDER', 'AGE', 'HEIGHT', 'WEIGHT'], attr, attr_numeric)
from pivot_example
group by id;

-- Pulling attributes out
SELECT id, p[1] as gender, p[2] as age, p[3] as height, p[4] as weight
FROM (select id, 
	  pivot_sum(ARRAY['GENDER', 'AGE', 'HEIGHT', 'WEIGHT'], attr, attr_int4) as p
      from pivot_example
      group by id) s1;

-- These should error, non-text pivots not yet implemented
select pivot_sum(ARRAY[0, 1, 32, 64], attr, attr_int2)    from pivot_example;
select pivot_sum(ARRAY[0, 1, 32, 64], attr, attr_int4)    from pivot_example;
select pivot_sum(ARRAY[0, 1, 32, 64], attr, attr_int8)    from pivot_example;
select pivot_sum(ARRAY[0, 1, 32, 64], attr, attr_float4)  from pivot_example;
select pivot_sum(ARRAY[0, 1, 32, 64], attr, attr_float8)  from pivot_example;
select pivot_sum(ARRAY[0, 1, 32, 64], attr, attr_numeric) from pivot_example;
select pivot_sum(ARRAY['0', '1', '32', '64'], attr_int2, attr_int2)    from pivot_example;
select pivot_sum(ARRAY['0', '1', '32', '64'], attr_int4, attr_int4)    from pivot_example;
select pivot_sum(ARRAY['0', '1', '32', '64'], attr_int8, attr_int8)    from pivot_example;
select pivot_sum(ARRAY['0', '1', '32', '64'], attr_float4, attr_float4)  from pivot_example;
select pivot_sum(ARRAY['0', '1', '32', '64'], attr_float8, attr_float8)  from pivot_example;
select pivot_sum(ARRAY['0', '1', '32', '64'], attr_numeric, attr_numeric) from pivot_example;
select pivot_sum(ARRAY[0, 1, 32, 64], attr_int2, attr_int2)    from pivot_example;
select pivot_sum(ARRAY[0, 1, 32, 64], attr_int4, attr_int4)    from pivot_example;
select pivot_sum(ARRAY[0, 1, 32, 64], attr_int8, attr_int8)    from pivot_example;
select pivot_sum(ARRAY[0, 1, 32, 64], attr_float4, attr_float4)  from pivot_example;
select pivot_sum(ARRAY[0, 1, 32, 64], attr_float8, attr_float8)  from pivot_example;
select pivot_sum(ARRAY[0, 1, 32, 64], attr_numeric, attr_numeric) from pivot_example;



create view unpivot_example as
SELECT id, p[1] as gender, p[2] as age, p[3] as height, p[4] as weight
FROM (select id, 
	  pivot_sum(ARRAY['GENDER', 'AGE', 'HEIGHT', 'WEIGHT'], attr, attr_int4) as p
      from pivot_example
      group by id) s1;

-- currently this has some problems
SELECT id, attr(unp), attr_value(unp)
FROM (select id, unpivot(ARRAY[gender, age, height, weight],
                         ARRAY['GENDER', 'AGE', 'HEIGHT', 'WEIGHT']) as unp
      from unpivot_example) s1;


drop view unpivot_example;
drop table pivot_example;