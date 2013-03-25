set TimeZone to 'US/Pacific';

-- Tests for Time Series features of 5.0+
-- Some of these tests could live elsewhere, but it seems good to collect them.

-- Interval-Interval division and remainder and associated overloads

select
    interval_interval_div(interval '365 days', interval '1 month') as "12 1/6",
    interval_interval_mod(interval '365 days', interval '1 month') as "5*24";

select
    interval_interval_div(interval '1 day', interval '8 hours') as "3",
    interval_interval_mod(interval '1 day', interval '8 hours') as "0";

select
    interval_interval_div(interval '2 hours', interval '-100 minutes') as "-1 1/5",
    interval_interval_mod(interval '2 hours', interval '-100 minutes') as "20";

-- generate_series with timestamp

select *
from generate_series(
	timestamp '2011-01-01 12:00:00',
	timestamp '2011-01-01 12:00:00',
	interval '1 year');

select *
from generate_series(
	timestamp '2011-01-01 12:00:00',
	timestamp '2012-01-01 12:00:00',
	interval '1 month');

select *
from generate_series(
	timestamp '2012-01-01 12:00:00',
	timestamp '2013-01-01 12:00:00',
	interval '1 month');

select *
from generate_series(
	timestamp '2011-01-01 12:00:00',
	timestamp '2011-01-31 12:00:00',
	interval '2 weeks');

select *
from generate_series(
	timestamp '2013-01-01 12:00:00',
	timestamp '2011-01-01 12:00:00',
	interval '-2 months');

select *
from generate_series(
	'2011-01-01 12:00:00',
	'2012-01-01 12:00:00',
	interval '0 sec');

-- generate_series with timestamptz

select *
from generate_series(
	timestamptz '2011-01-01 12:00:00 UTC',
	timestamptz '2011-01-01 12:00:00 UTC',
	interval '1 year');

select *
from generate_series(
	timestamptz '2011-01-01 12:00:00 UTC',
	timestamptz '2012-01-01 12:00:00 UTC',
	interval '1 month');

select *
from generate_series(
	timestamptz '2012-01-01 12:00:00 UTC',
	timestamptz '2013-01-01 12:00:00 UTC',
	interval '1 month');

select *
from generate_series(
	timestamptz '2011-01-01 12:00:00 UTC',
	timestamptz '2011-01-31 12:00:00 UTC',
	interval '2 weeks');

select *
from generate_series(
	timestamptz '2013-01-01 12:00:00 UTC',
	timestamptz '2011-01-01 12:00:00 UTC',
	interval '-2 months');

select *
from generate_series(
	'2011-01-01 12:00:00 UTC',
	'2012-01-01 12:00:00 UTC',
	interval '0 sec');

-- interval_bound with numeric

select interval_bound(0, 0);
select interval_bound(0, -1);

select v, w, r, s,
    interval_bound(v, w) as "normal",
    interval_bound(v, w, s) as "shifted s",
    interval_bound(v, w, s, r) as "and registered to r"
from ( values
        (10, 1, 0.5, 4),
        (10, 0.5, -100, null),
        (0.5, 10, -1, -1),
        (-100, 100, 10, 1),
        (-101, 10, null, 10),
        (5, 2, -100.5, 1),
        (null, 10, 0, 0),
        (55, null, 20, 0),
        ('NaN', 10, 10, 0),
        (45.6, 'NaN', 5.5, 2),
        (31, 10, 'NaN', 0)
    ) r(v,w,r,s);

-- interval_bound with timestamp

select interval_bound(timestamp 'now', interval '0 hours');
select interval_bound(timestamp 'now', interval '1 day - 24 hours');
select interval_bound(timestamp 'infinity', interval '1 month');
select interval_bound(timestamp '-infinity', interval '1 month');
select interval_bound(timestamp 'now', interval '2 hours', null, timestamp 'infinity');

select v, w, r, s,
    interval_bound(v, w) as "normal",
    interval_bound(v, w, s) as "shifted s",
    interval_bound(v, w, s, r) as "and registered to r"
from ( values
        (timestamp '2012-01-12 10:00:10', interval '1 week', timestamp '2012-04-02 00:00:00', 4),
        (timestamp '1929-10-29 22:33:44.55', interval '1 year', timestamp '1991-01-17 02:34:56.78', null),
        (timestamp '1991-01-17 02:34:56.78', interval '1 year -1 month', timestamp '1776-07-04 12:34:56', -1),
        (timestamp '2100-03-01 11:11:11.11', interval '100 days', timestamp '1929-10-29 22:33:44.55', 1),
        (timestamp '1776-07-04 12:34:56', interval '1 month', timestamp '2012-04-01 00:00:00', 1),
        (null::timestamp, interval '1 week', timestamp '1911-09-09 15:16:17', 3),
        (timestamp '1999-10-30 13:01:01', null::interval, timestamp '1970-04-05 12:00:00', 1),
        (timestamp '1999-10-30 13:01:01', interval '1 day', null, 1)
    ) r(v,w,r,s);

-- interval_bound with timestamptz

select interval_bound(timestamptz 'now', interval '0 hours');
select interval_bound(timestamptz 'now', interval '1 day - 24 hours');
select interval_bound(timestamptz 'infinity', interval '1 month');
select interval_bound(timestamptz '-infinity', interval '1 month');
select interval_bound(timestamptz 'now', interval '2 hours', null, timestamptz 'infinity');

select v, w, r, s,
    interval_bound(v, w) as "normal",
    interval_bound(v, w, s) as "shifted s",
    interval_bound(v, w, s, r) as "and registered to r"
from ( values
        (timestamptz '2012-01-12 10:00:10', interval '1 week', timestamptz '2012-04-02 00:00:00 US/Eastern', 4),
        (timestamptz '1929-10-29 22:33:44.55', interval '1 year', timestamptz '1991-01-17 02:34:56.78', null),
        (timestamptz '1991-01-17 02:34:56.78', interval '1 year -1 month', timestamptz '1776-07-04 12:34:56 US/Eastern', -1),
        (timestamptz '2100-03-01 11:11:11.11 UTC', interval '100 days', timestamptz '1929-10-29 22:33:44.55', 1),
        (timestamptz '1776-07-04 12:34:56', interval '1 month', timestamptz '2012-04-01 00:00:00 UTC', 1),
        (null::timestamp, interval '1 week', timestamp '1911-09-09 15:16:17', 3),
        (timestamptz '1999-10-30 13:01:01', null::interval, timestamptz '1970-04-05 12:00:00', 1),
        (timestamptz '1999-10-30 13:01:01', interval '1 day', null, 1)
    ) r(v,w,r,s);
