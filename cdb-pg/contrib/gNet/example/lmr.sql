drop table mydata;
create table mydata (x1 int, x2 int, y int) distributed by (x1,x2);
insert into mydata values(1,1,1);
insert into mydata values(2,1,2);
insert into mydata values(3,2,3);
insert into mydata values(4,2,4);
insert into mydata values(5,3,3);
insert into mydata values(6,3,7);
insert into mydata values(7,6,10);

\timing

-- built in
select regr_intercept(y,x1), regr_slope(y,x1), regr_r2(y,x1)
FROM mydata;


-- using gNet features
select mregr_coef(y, ARRAY[1, x1]), mregr_r2(y, ARRAY[1, x1])
FROM mydata;

-- built in
select regr_intercept(y,x2), regr_slope(y,x2), regr_r2(y, x2)
FROM mydata;

-- using gNet features
select mregr_coef(y, ARRAY[1, x2]), mregr_r2(y, ARRAY[1, x2])
FROM mydata;


-- using gNet features we can also do multiple linear regression
--   * just add extra parameters to the X array
select mregr_coef(y, ARRAY[1, x1, x2]), mregr_r2(y, ARRAY[1, x1, x2])
FROM mydata;


-- example including UNPIVOT
SELECT attr(tuple), attr_value(tuple)
FROM (SELECT unpivot(mregr_coef, ARRAY['INTERCEPT', 'X1', 'X2']) as tuple
      FROM (SELECT mregr_coef(y, ARRAY[1, x1, x2]) FROM mydata) q1) q2;
\timing