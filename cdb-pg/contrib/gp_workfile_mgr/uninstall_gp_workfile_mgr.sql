
\set WORKFILE_SCHEMA workfile

SET search_path = :WORKFILE_SCHEMA;
BEGIN;

DROP VIEW gp_workfile_usage_per_query;

DROP VIEW gp_workfile_entries;
DROP FUNCTION gp_workfile_entries();

DROP FUNCTION gp_workfile_cache_clear();
DROP FUNCTION gp_workfile_cache_clear_segment(content int);
DROP FUNCTION gp_workfile_cache_clear_f(content int); 

DROP FUNCTION gp_workfile_mgr_test_allsegs(text);
DROP FUNCTION gp_workfile_mgr_test(text);

DROP VIEW gp_workfile_usage_per_segment;
DROP FUNCTION gp_workfile_usage_per_segment_f();

COMMIT; 
