create table pivot_example (id int, attr text, attr_val int)
distributed by (id);
insert into pivot_example values (1, 'GENDER', 0);
insert into pivot_example values (1, 'AGE', 32);
insert into pivot_example values (1, 'HEIGHT', 68);
insert into pivot_example values (1, 'WEIGHT', 132);
insert into pivot_example values (2, 'GENDER', 1);
insert into pivot_example values (2, 'AGE', 29);
insert into pivot_example values (2, 'HEIGHT', 64);
insert into pivot_example values (2, 'WEIGHT', 112);
insert into pivot_example values (3, 'GENDER', 0);
insert into pivot_example values (3, 'AGE', 36);
insert into pivot_example values (3, 'HEIGHT', 69);
insert into pivot_example values (3, 'WEIGHT', 164);


select * from pivot_example;

select id, pivot_sum(ARRAY['GENDER', 'AGE', 'HEIGHT', 'WEIGHT'], attr, attr_val)
from pivot_example
group by id;


SELECT id, p[1] as gender, p[2] as age, p[3] as height, p[4] as weight
FROM (select id, 
	  pivot_sum(ARRAY['GENDER', 'AGE', 'HEIGHT', 'WEIGHT'], attr, attr_val) as p
      from pivot_example
      group by id) s1;

create view unpivot_example as
SELECT id, p[1] as gender, p[2] as age, p[3] as height, p[4] as weight
FROM (select id, 
	  pivot_sum(ARRAY['GENDER', 'AGE', 'HEIGHT', 'WEIGHT'], attr, attr_val) as p
      from pivot_example
      group by id) s1;

-- currently this has some problems
SELECT id, attr(unp), attr_value(unp)
FROM (select id, unpivot(ARRAY[gender, age, height, weight],
                         ARRAY['GENDER', 'AGE', 'HEIGHT', 'WEIGHT']) as unp
      from unpivot_example) s1;


drop view unpivot_example;
drop table pivot_example;