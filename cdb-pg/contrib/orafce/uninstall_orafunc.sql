BEGIN;
DROP FUNCTION IF EXISTS nvl(anyelement, anyelement);

DROP FUNCTION IF EXISTS decode(anyelement, anyelement, text);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, text, text);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, text, anyelement, text);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, text, anyelement, text, text);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, text, anyelement, text, anyelement, text);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, text, anyelement, text, anyelement, text, text);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, bpchar);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, bpchar, bpchar);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, bpchar, anyelement, bpchar);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, bpchar, anyelement, bpchar, bpchar);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, bpchar, anyelement, bpchar, anyelement, bpchar);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, bpchar, anyelement, bpchar, anyelement, bpchar, bpchar);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, integer);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, integer, integer);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, integer, anyelement, integer);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, integer, anyelement, integer, integer);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, integer, anyelement, integer, anyelement, integer);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, integer, anyelement, integer, anyelement, integer, integer);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, bigint);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, bigint, bigint);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, bigint, anyelement, bigint);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, bigint, anyelement, bigint, bigint);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, bigint, anyelement, bigint, anyelement, bigint);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, bigint, anyelement, bigint, anyelement, bigint, bigint);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, numeric);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, numeric, numeric);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, numeric, anyelement, numeric);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, numeric, anyelement, numeric, numeric);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, numeric, anyelement, numeric, anyelement, numeric);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, numeric, anyelement, numeric, anyelement, numeric, numeric);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, date);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, date, date);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, date, anyelement, date);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, date, anyelement, date, date);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, date, anyelement, date, anyelement, date);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, date, anyelement, date, anyelement, date, date);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, time);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, time, time);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, time, anyelement, time);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, time, anyelement, time, time);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, time, anyelement, time, anyelement, time);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, time, anyelement, time, anyelement, time, time);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, timestamp);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, timestamp, timestamp);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, timestamp, anyelement, timestamp);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, timestamp, anyelement, timestamp, timestamp);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, timestamp, anyelement, timestamp, anyelement, timestamp);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, timestamp, anyelement, timestamp, anyelement, timestamp, timestamp);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, timestamptz);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, timestamptz, timestamptz);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, timestamptz, anyelement, timestamptz);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, timestamptz, anyelement, timestamptz, timestamptz);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, timestamptz, anyelement, timestamptz, anyelement, timestamptz);
DROP FUNCTION IF EXISTS decode(anyelement, anyelement, timestamptz, anyelement, timestamptz, anyelement, timestamptz, timestamptz);

DROP FUNCTION IF EXISTS pg_catalog.add_months(day date, value int);

DROP FUNCTION IF EXISTS pg_catalog.last_day(value date);

DROP FUNCTION IF EXISTS pg_catalog.next_day(value date, weekday text);
DROP FUNCTION IF EXISTS pg_catalog.next_day(value date, weekday integer);

DROP FUNCTION IF EXISTS pg_catalog.months_between(date1 date, date2 date);

DROP FUNCTION IF EXISTS pg_catalog.trunc(value date, fmt text);
DROP FUNCTION IF EXISTS pg_catalog.trunc(value timestamp with time zone, fmt text);
DROP FUNCTION IF EXISTS pg_catalog.trunc(value timestamp with time zone);
DROP FUNCTION IF EXISTS pg_catalog.trunc(value date);

DROP FUNCTION IF EXISTS pg_catalog.round(value timestamp with time zone, fmt text);
DROP FUNCTION IF EXISTS pg_catalog.round(value timestamp with time zone);
DROP FUNCTION IF EXISTS pg_catalog.round(value date);
DROP FUNCTION IF EXISTS pg_catalog.round(value date, fmt text);

DROP FUNCTION IF EXISTS pg_catalog.instr(str text, patt text, start int, nth int);
DROP FUNCTION IF EXISTS pg_catalog.instr(str text, patt text, start int);
DROP FUNCTION IF EXISTS pg_catalog.instr(str text, patt text);

COMMIT;
