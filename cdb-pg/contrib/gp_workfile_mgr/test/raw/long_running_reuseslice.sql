
DROP TABLE IF EXISTS testq;
CREATE TABLE testq (a int, b int, c int, d int, e int, f int, g int, h int)
 DISTRIBUTED randomly;

INSERT INTO testq SELECT i,i,i,i,i,i,i,i FROM generate_series (0,999999) i; 

SET statement_mem=2048;
set gp_resqueue_print_operator_memory_limits=on;
SET gp_workfile_caching=true; 
SET gp_workfile_caching_loglevel="LOG"; 
set gp_workfile_type_hashjoin=bfz; 

explain analyze 
select max(a) over (partition by b order by c rows between 100000 preceding and 100000 following) 
from testq;  

select runstmt('select gp_workfile_cache_clear()') from gp_dist_random('gp_id') where gp_segment_id=0;

set log_min_messages="debug2";