-- Adjust this setting to control where the objects get created.
SET search_path = public;

BEGIN;

-- NVL and DECODE

CREATE FUNCTION nvl(anyelement, anyelement)
RETURNS anyelement
AS 'MODULE_PATHNAME','ora_nvl'
LANGUAGE C IMMUTABLE;
COMMENT ON FUNCTION nvl(anyelement, anyelement) IS '';

CREATE FUNCTION decode(anyelement, anyelement, text)
RETURNS text
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, text, text)
RETURNS text
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, text, anyelement, text)
RETURNS text
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, text, anyelement, text, text)
RETURNS text
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, text, anyelement, text, anyelement, text)
RETURNS text
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, text, anyelement, text, anyelement, text, text)
RETURNS text
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, bpchar)
RETURNS bpchar
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, bpchar, bpchar)
RETURNS bpchar
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, bpchar, anyelement, bpchar)
RETURNS bpchar
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, bpchar, anyelement, bpchar, bpchar)
RETURNS bpchar
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, bpchar, anyelement, bpchar, anyelement, bpchar)
RETURNS bpchar
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, bpchar, anyelement, bpchar, anyelement, bpchar, bpchar)
RETURNS bpchar
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, integer, integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, integer, anyelement, integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, integer, anyelement, integer, integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, integer, anyelement, integer, anyelement, integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, integer, anyelement, integer, anyelement, integer, integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, bigint, bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, bigint, anyelement, bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, bigint, anyelement, bigint, bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, bigint, anyelement, bigint, anyelement, bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, bigint, anyelement, bigint, anyelement, bigint, bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, numeric)
RETURNS numeric
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, numeric, numeric)
RETURNS numeric
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, numeric, anyelement, numeric)
RETURNS numeric
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, numeric, anyelement, numeric, numeric)
RETURNS numeric
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, numeric, anyelement, numeric, anyelement, numeric)
RETURNS numeric
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, numeric, anyelement, numeric, anyelement, numeric, numeric)
RETURNS numeric
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, date)
RETURNS date
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, date, date)
RETURNS date
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, date, anyelement, date)
RETURNS date
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, date, anyelement, date, date)
RETURNS date
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, date, anyelement, date, anyelement, date)
RETURNS date
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, date, anyelement, date, anyelement, date, date)
RETURNS date
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, time)
RETURNS time
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, time, time)
RETURNS time
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, time, anyelement, time)
RETURNS time
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, time, anyelement, time, time)
RETURNS time
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, time, anyelement, time, anyelement, time)
RETURNS time
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, time, anyelement, time, anyelement, time, time)
RETURNS time
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, timestamp)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, timestamp, timestamp)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, timestamp, anyelement, timestamp)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, timestamp, anyelement, timestamp, timestamp)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, timestamp, anyelement, timestamp, anyelement, timestamp)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, timestamp, anyelement, timestamp, anyelement, timestamp, timestamp)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, timestamptz)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, timestamptz, timestamptz)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, timestamptz, anyelement, timestamptz)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, timestamptz, anyelement, timestamptz, timestamptz)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, timestamptz, anyelement, timestamptz, anyelement, timestamptz)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION decode(anyelement, anyelement, timestamptz, anyelement, timestamptz, anyelement, timestamptz, timestamptz)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

COMMIT;
