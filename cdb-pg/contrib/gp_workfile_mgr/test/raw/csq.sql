
create table t1 (x int, y int); 
create table t2 (x int, y int); 


explain select * from t1 where t1.x >ALL (select t2.x from t2 where t2.y=t1.y);