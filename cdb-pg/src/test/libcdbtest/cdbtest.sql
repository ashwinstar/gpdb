

CREATE TYPE __getrowsaschunks AS (rownum integer, chunknum integer, chunkdata bytea);

CREATE OR REPLACE FUNCTION getrowsaschunks(text) RETURNS SETOF __getrowsaschunks
    AS 'libcdbtest.so', 'GetRowsAsChunks__text1'
    LANGUAGE C IMMUTABLE STRICT;


