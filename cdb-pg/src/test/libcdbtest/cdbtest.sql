

-- These declarations are for the functions in chunkfuncs.c

CREATE TYPE __getserializedrows AS (reportedsize integer, serialized bytea);

CREATE OR REPLACE FUNCTION getserializedrows(text) RETURNS SETOF __getserializedrows
    AS 'libcdbtest.so', 'GetSerializedRows__text1'
    LANGUAGE C IMMUTABLE STRICT;


CREATE TYPE __getchunkedrows AS (rownum integer, chunknum integer, chunksize integer, chunkdata bytea);

CREATE OR REPLACE FUNCTION getchunkedrows(text) RETURNS SETOF __getchunkedrows
    AS 'libcdbtest.so', 'GetChunkedRows__text1'
    LANGUAGE C IMMUTABLE STRICT;


CREATE OR REPLACE FUNCTION getdeserializedrows(text, regtype) RETURNS SETOF RECORD
    AS 'libcdbtest.so', 'GetDeserializedRows__text1_regtype1'
    LANGUAGE C IMMUTABLE STRICT;


CREATE OR REPLACE FUNCTION getdechunkedrows(text, regtype) RETURNS SETOF RECORD
    AS 'libcdbtest.so', 'GetDechunkedRows__text1_regtype1'
    LANGUAGE C IMMUTABLE STRICT;

-- Plan and Query Serialization functions
CREATE OR REPLACE FUNCTION cdb_exec_plan(text, text, text) RETURNS boolean
    AS 'libcdbtest.so', 'cdb_exec_plan'
    LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION cdb_serialize_plan(text) RETURNS text
    AS 'libcdbtest.so', 'cdb_serialize_plan'
    LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION cdb_serialize_query(text) RETURNS text
    AS 'libcdbtest.so', 'cdb_serialize_query'
    LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION cdb_exec_indirect(text) RETURNS SETOF RECORD
    AS 'libcdbtest.so', 'cdb_exec_indirect'
    LANGUAGE C IMMUTABLE STRICT;
