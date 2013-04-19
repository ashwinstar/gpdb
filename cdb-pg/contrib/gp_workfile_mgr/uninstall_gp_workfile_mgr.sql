
SET search_path = workfile;

BEGIN;

DROP VIEW gp_workfile_usage_per_query;
DROP VIEW gp_workfile_usage_per_segment;
DROP VIEW gp_workfile_entries;
DROP FUNCTION gp_workfile_entries();

DROP FUNCTION gp_workfile_cache_clear();
DROP FUNCTION gp_workfile_cache_clear_segment(content int);
DROP FUNCTION gp_workfile_cache_clear_f(content int); 

DROP FUNCTION gp_workfile_mgr_test_allsegs(text);
DROP FUNCTION gp_workfile_mgr_test(text);

DROP SCHEMA workfile;

COMMIT; 
