drop table if exists test;
create table test (a int, b svec) DISTRIBUTED BY (a);

insert into test (select 1,gp_extract_feature_histogram('{"one","two","three","four","five","six"}','{"twe","four","five","six","one","three","two","one"}'));
insert into test (select 2,gp_extract_feature_histogram('{"one","two","three","four","five","six"}','{"the","brown","cat","ran","across","three","dogs"}'));
insert into test (select 3,gp_extract_feature_histogram('{"one","two","three","four","five","six"}','{"two","four","five","six","one","three","two","one"}'));

-- Test the equals operator (should be only 3 rows)
select a,b::float8[] cross_product_equals from (select a,b from test) foo where b = foo.b order by a;

drop table if exists test2;
create table test2 as select * from test DISTRIBUTED BY (a);
-- Test the plus operator (should be 9 rows)
select (t1.b+t2.b)::float8[] cross_product_sum from test t1, test2 t2 order by t1.a;

-- Test ORDER BY
select (t1.b+t2.b)::float8[] cross_product_sum, svec_l2norm(t1.b+t2.b) l2norm, (t1.b+t2.b) sparse_vector from test t1, test2 t2 order by 3;

 select (sum(t1.b))::float8[] as features_sum from test t1;
-- Test the div operator
 select (t1.b/(select sum(b) from test))::float8[] as weights from test t1 order by a;
-- Test the * operator
 select t1.b %*% (t1.b/(select sum(b) from test)) as raw_score from test t1 order by a;
-- Test the * and l2norm operators
 select (t1.b %*% (t1.b/(select sum(b) from test))) / (svec_l2norm(t1.b) * svec_l2norm((select sum(b) from test))) as norm_score from test t1 order by a;
-- Test the ^ and l1norm operators
select ('{1,2}:{20.,10.}'::svec)^('{1}:{3.}'::svec);
 select (t1.b %*% (t1.b/(select sum(b) from test))) / (svec_l1norm(t1.b) * svec_l1norm((select sum(b) from test))) as norm_score from test t1 order by a;

-- Test the multi-concatenation and show sizes compared with a normal array
drop table if exists corpus_proj;
drop table if exists corpus_proj_array;
create table corpus_proj as (select 10000 *|| ('{45,2,35,4,15,1}:{0,1,0,1,0,2}'::svec) result );
create table corpus_proj_array as (select result::float8[] from corpus_proj) distributed randomly;
-- Calculate on-disk size of sparse vector
select pg_size_pretty(pg_total_relation_size('corpus_proj'));
-- Calculate on-disk size of normal array
select pg_size_pretty(pg_total_relation_size('corpus_proj_array'));
\timing
-- Calculate L1 norm from sparse vector
select l1norm(result) from corpus_proj;
-- Calculate L1 norm from float8[]
select l1norm(result) from corpus_proj_array;
-- Calculate L2 norm from sparse vector
select l2norm(result) from corpus_proj;
-- Calculate L2 norm from float8[]
select l2norm(result) from corpus_proj_array;


drop table corpus_proj;
drop table corpus_proj_array;
drop table test;
drop table test2;
