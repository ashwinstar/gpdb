\echo Use "CREATE EXTENSION gp_snapshot" to load this file. \quit

CREATE FUNCTION gp_start_snapshot() RETURNS void
AS '$libdir/gp_snapshot', 'gp_start_snapshot'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gp_stop_snapshot() RETURNS void
AS '$libdir/gp_snapshot', 'gp_stop_snapshot'
LANGUAGE C IMMUTABLE STRICT;
