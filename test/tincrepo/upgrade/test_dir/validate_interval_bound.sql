select interval_interval_div(interval '365 days', interval '1 month') as "12 1/6";

select interval_interval_mod(interval '365 days', interval '1 month') as "5*24";

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
    ) r(v,w,r,s)
order by v, w, r, s;

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
        (timestamp '1999-10-30 13:01:01', null::interval, timestamp '1970-04-05 12:00:00', 1)
    ) r(v,w,r,s)
order by v, w, r, s;

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
        (timestamp '1999-10-30 13:01:01', null::interval, timestamp '1970-04-05 12:00:00', 1)
    ) r(v,w,r,s)
order by v, w, r, s;

select *
from generate_series(
	timestamptz '2013-01-01 12:00:00 UTC',
	timestamptz '2011-01-01 12:00:00 UTC',
	interval '-2 months');
