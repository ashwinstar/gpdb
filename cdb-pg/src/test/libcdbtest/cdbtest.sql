


CREATE TYPE __getserializedrows AS (reportedsize integer, serialized bytea);

CREATE OR REPLACE FUNCTION getserializedrows(text) RETURNS SETOF __getserializedrows
    AS 'libcdbtest.so', 'GetSerializedRows__text1'
    LANGUAGE C IMMUTABLE STRICT;



CREATE TYPE __getrowsaschunks AS (rownum integer, chunknum integer, chunkdata bytea);

CREATE OR REPLACE FUNCTION getrowsaschunks(text) RETURNS SETOF __getrowsaschunks
    AS 'libcdbtest.so', 'GetRowsAsChunks__text1'
    LANGUAGE C IMMUTABLE STRICT;


CREATE OR REPLACE FUNCTION getdeserializedrows(text, regtype) RETURNS SETOF RECORD
    AS 'libcdbtest.so', 'GetDeserializedRows__text1_regtype1'
    LANGUAGE C IMMUTABLE STRICT;


-- CREATE OR REPLACE FUNCTION getdechunkedrow(text, regtype) RETURNS SETOF RECORD
--     AS 'libcdbtest.so', 'GetDechunkedRow__text1_regtype1'
--     LANGUAGE C IMMUTABLE STRICT;

