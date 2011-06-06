-- Adjust this setting to control where the objects get created.
SET search_path = public;

BEGIN;

-- NVL
CREATE OR REPLACE FUNCTION nvl(anyelement, anyelement)
RETURNS anyelement
AS 'MODULE_PATHNAME','ora_nvl'
LANGUAGE C IMMUTABLE;
COMMENT ON FUNCTION nvl(anyelement, anyelement) IS '';

-- DECODE
CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, text)
RETURNS text
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, text, text)
RETURNS text
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, text, anyelement, text)
RETURNS text
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, text, anyelement, text, text)
RETURNS text
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, text, anyelement, text, anyelement, text)
RETURNS text
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, text, anyelement, text, anyelement, text, text)
RETURNS text
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, bpchar)
RETURNS bpchar
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, bpchar, bpchar)
RETURNS bpchar
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, bpchar, anyelement, bpchar)
RETURNS bpchar
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, bpchar, anyelement, bpchar, bpchar)
RETURNS bpchar
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, bpchar, anyelement, bpchar, anyelement, bpchar)
RETURNS bpchar
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, bpchar, anyelement, bpchar, anyelement, bpchar, bpchar)
RETURNS bpchar
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, integer, integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, integer, anyelement, integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, integer, anyelement, integer, integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, integer, anyelement, integer, anyelement, integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, integer, anyelement, integer, anyelement, integer, integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, bigint, bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, bigint, anyelement, bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, bigint, anyelement, bigint, bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, bigint, anyelement, bigint, anyelement, bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, bigint, anyelement, bigint, anyelement, bigint, bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, numeric)
RETURNS numeric
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, numeric, numeric)
RETURNS numeric
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, numeric, anyelement, numeric)
RETURNS numeric
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, numeric, anyelement, numeric, numeric)
RETURNS numeric
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, numeric, anyelement, numeric, anyelement, numeric)
RETURNS numeric
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, numeric, anyelement, numeric, anyelement, numeric, numeric)
RETURNS numeric
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, date)
RETURNS date
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, date, date)
RETURNS date
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, date, anyelement, date)
RETURNS date
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, date, anyelement, date, date)
RETURNS date
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, date, anyelement, date, anyelement, date)
RETURNS date
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, date, anyelement, date, anyelement, date, date)
RETURNS date
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, time)
RETURNS time
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, time, time)
RETURNS time
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, time, anyelement, time)
RETURNS time
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, time, anyelement, time, time)
RETURNS time
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, time, anyelement, time, anyelement, time)
RETURNS time
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, time, anyelement, time, anyelement, time, time)
RETURNS time
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, timestamp)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, timestamp, timestamp)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, timestamp, anyelement, timestamp)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, timestamp, anyelement, timestamp, timestamp)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, timestamp, anyelement, timestamp, anyelement, timestamp)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, timestamp, anyelement, timestamp, anyelement, timestamp, timestamp)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, timestamptz)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, timestamptz, timestamptz)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, timestamptz, anyelement, timestamptz)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, timestamptz, anyelement, timestamptz, timestamptz)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, timestamptz, anyelement, timestamptz, anyelement, timestamptz)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION decode(anyelement, anyelement, timestamptz, anyelement, timestamptz, anyelement, timestamptz, timestamptz)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'ora_decode'
LANGUAGE C IMMUTABLE;

-- ADD_MONTHS
CREATE OR REPLACE FUNCTION pg_catalog.add_months(day date, value int)
RETURNS date
AS 'MODULE_PATHNAME'
LANGUAGE 'C' IMMUTABLE STRICT;

-- LAST_DAY
CREATE OR REPLACE FUNCTION pg_catalog.last_day(value date)
RETURNS date
AS 'MODULE_PATHNAME'
LANGUAGE 'C' IMMUTABLE STRICT;

-- NEXT_DAY
CREATE OR REPLACE FUNCTION pg_catalog.next_day(value date, weekday text)
RETURNS date
AS 'MODULE_PATHNAME'
LANGUAGE 'C' IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION pg_catalog.next_day(value date, weekday integer)
RETURNS date
AS 'MODULE_PATHNAME', 'next_day_by_index'
LANGUAGE 'C' IMMUTABLE STRICT;

-- MONTHS_BETWEEN
CREATE OR REPLACE FUNCTION pg_catalog.months_between(date1 date, date2 date)
RETURNS numeric
AS 'MODULE_PATHNAME'
LANGUAGE 'C' IMMUTABLE STRICT;

-- TRUNC
CREATE OR REPLACE FUNCTION pg_catalog.trunc(value date, fmt text)
RETURNS date
AS 'MODULE_PATHNAME','ora_date_trunc'
LANGUAGE 'C' IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION pg_catalog.trunc(value timestamp with time zone, fmt text)
RETURNS timestamp with time zone
AS 'MODULE_PATHNAME', 'ora_timestamptz_trunc'
LANGUAGE 'C' IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION pg_catalog.trunc(value timestamp with time zone)
RETURNS timestamp with time zone
AS $$ SELECT pg_catalog.trunc($1, 'DDD'); $$
LANGUAGE 'SQL' IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION pg_catalog.trunc(value date)
RETURNS date
AS $$ SELECT $1; $$
LANGUAGE 'SQL' IMMUTABLE STRICT;

-- ROUND
CREATE OR REPLACE FUNCTION pg_catalog.round(value date, fmt text)
RETURNS date
AS 'MODULE_PATHNAME','ora_date_round'
LANGUAGE 'C' IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION pg_catalog.round(value timestamp with time zone, fmt text)
RETURNS timestamp with time zone
AS 'MODULE_PATHNAME','ora_timestamptz_round'
LANGUAGE 'C' IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION pg_catalog.round(value timestamp with time zone)
RETURNS timestamp with time zone
AS $$ SELECT pg_catalog.round($1, 'DDD'); $$
LANGUAGE 'SQL' IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION pg_catalog.round(value date)
RETURNS date
AS $$ SELECT $1; $$
LANGUAGE 'SQL' IMMUTABLE STRICT;

-- INSTR
CREATE OR REPLACE FUNCTION pg_catalog.instr(str text, patt text, start int, nth int)
RETURNS int
AS 'MODULE_PATHNAME','plvstr_instr4'
LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION pg_catalog.instr(str text, patt text, start int)
RETURNS int
AS 'MODULE_PATHNAME','plvstr_instr3'
LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION pg_catalog.instr(str text, patt text)
RETURNS int
AS 'MODULE_PATHNAME','plvstr_instr2'
LANGUAGE C IMMUTABLE STRICT;

COMMIT;
