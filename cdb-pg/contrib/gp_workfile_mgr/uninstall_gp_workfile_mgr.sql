-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP VIEW gp_workfile_mgr_cache_stats;
DROP FUNCTION gp_workfile_mgr_cache_stats();

DROP VIEW gp_workfile_mgr_cache_entries;
DROP FUNCTION gp_workfile_mgr_cache_entries();

DROP FUNCTION gp_workfile_mgr_clear_allsegs();
DROP FUNCTION gp_workfile_mgr_clear();

DROP FUNCTION gp_workfile_mgr_test_allsegs(text);
DROP FUNCTION gp_workfile_mgr_test(text);

DROP VIEW gp_workfile_mgr_diskspace;
DROP FUNCTION gp_workfile_mgr_used_diskspace();
