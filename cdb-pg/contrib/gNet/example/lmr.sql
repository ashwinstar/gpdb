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
select regr_intercept(y,x1), regr_slope(y,x1) FROM mydata;

-- using gNet features
select regr_R(sum(matrix_multiply(matrix_transpose(x),x)),
              sum(matrix_multiply(x,y)))
FROM (SELECT ARRAY[[1, x1]] as x, y from mydata) s1;

-- built in
select regr_intercept(y,x2), regr_slope(y,x2) FROM mydata;

-- using gNet features
select regr_R(sum(matrix_multiply(matrix_transpose(x),x)),
              sum(matrix_multiply(x,y)))
FROM (SELECT ARRAY[[1, x2]] as x, y from mydata) s1;


-- using gNet features we can also do multiple linear regression
--   * just add extra parameters to the X array
select regr_R(sum(matrix_multiply(matrix_transpose(x),x)),
              sum(matrix_multiply(x,y)))
FROM (SELECT ARRAY[[1, x1, x2]] as x, y from mydata) s1;


-- example including UNPIVOT
SELECT attr(tuple), attr_value(tuple)
FROM (SELECT unpivot(calculated_regression, ARRAY['INTERCEPT', 'X1', 'X2']) as tuple
      FROM (SELECT regr_R(sum(matrix_multiply(matrix_transpose(x),x)),
                          sum(matrix_multiply(x,y))) as calculated_regression
            FROM (SELECT ARRAY[[1, x1, x2]] as x, y from mydata) s1) s2) s3;
\timing