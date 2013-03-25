select oid, * from gp_dist_random('pg_type') where typnamespace <> (select oid from pg_namespace where nspname='gp_toolkit') order by oid; -- Exclude gp_toolkit
