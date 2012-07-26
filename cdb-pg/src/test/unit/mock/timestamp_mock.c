/*
 * Manually created mocking source
 *
 * This file is not automatically updated by generate_mocks.py
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <ctype.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <sys/time.h>
#include "access/hash.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "parser/scansup.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datetime.h"

TimestampTz     PgStartTime;
TimestampTz     PgReloadTime;
Datum 
timestamp_in(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_out(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_recv(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_send(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptypmodin(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptypmodout(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_interval_bound(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_interval_bound(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_scale(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_in(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_out(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_recv(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_send(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptztypmodin(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptztypmodout(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_scale(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_in(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_out(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_recv(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_send(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
intervaltypmodin(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
intervaltypmodout(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_scale(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
now(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
statement_timestamp(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
clock_timestamp(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
pg_postmaster_start_time(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
pgsql_postmaster_start_time(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
pg_conf_load_time(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

TimestampTz 
GetCurrentTimestamp(void)
{


	return (TimestampTz) mock();
}

void 
TimestampDifference(TimestampTz start_time, TimestampTz stop_time, long *secs, int *microsecs)
{
	check_expected(start_time);
	check_expected(stop_time);
	check_expected(secs);
	check_expected(microsecs);
	optional_assignment(secs);
	optional_assignment(microsecs);
	mock();
}

bool 
TimestampDifferenceExceeds(TimestampTz start_time, TimestampTz stop_time, int msec)
{
	check_expected(start_time);
	check_expected(stop_time);
	check_expected(msec);

	return (bool) mock();
}

TimestampTz 
time_t_to_timestamptz(pg_time_t tm)
{
	check_expected(tm);

	return (TimestampTz) mock();
}

pg_time_t 
timestamptz_to_time_t(TimestampTz t)
{
	check_expected(t);

	return (pg_time_t) mock();
}

const char     *
timestamptz_to_str(TimestampTz t)
{
	check_expected(t);

	return (char *) (unsigned) mock();
}

void 
dt2time(Timestamp jd, int *hour, int *min, int *sec, fsec_t * fsec)
{
	check_expected(jd);
	check_expected(hour);
	check_expected(min);
	check_expected(sec);
	check_expected(fsec);
	optional_assignment(hour);
	optional_assignment(min);
	optional_assignment(sec);
	optional_assignment(fsec);
	mock();
}

int 
timestamp2tm(Timestamp dt, int *tzp, struct pg_tm * tm, fsec_t * fsec, char **tzn, pg_tz * attimezone)
{
	check_expected(dt);
	check_expected(tzp);
	check_expected(tm);
	check_expected(fsec);
	check_expected(tzn);
	check_expected(attimezone);
	optional_assignment(tzp);
	optional_assignment(tm);
	optional_assignment(fsec);
	optional_assignment(tzn);
	optional_assignment(attimezone);
	return (int) mock();
}

int 
tm2timestamp(struct pg_tm * tm, fsec_t fsec, int *tzp, Timestamp * result)
{
	check_expected(tm);
	check_expected(fsec);
	check_expected(tzp);
	check_expected(result);
	optional_assignment(tm);
	optional_assignment(tzp);
	optional_assignment(result);
	return (int) mock();
}

int 
tm2interval(struct pg_tm * tm, fsec_t fsec, Interval * span)
{
	check_expected(tm);
	check_expected(fsec);
	check_expected(span);
	optional_assignment(tm);
	optional_assignment(span);
	return (int) mock();
}

Datum 
timestamp_finite(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_finite(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

void 
GetEpochTime(struct pg_tm * tm)
{
	check_expected(tm);
	optional_assignment(tm);
	mock();
}

Timestamp 
SetEpochTimestamp(void)
{


	return (Timestamp) mock();
}

int 
timestamp_cmp_internal(Timestamp dt1, Timestamp dt2)
{
	check_expected(dt1);
	check_expected(dt2);

	return (int) mock();
}

Datum 
timestamp_eq(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_ne(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_lt(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_gt(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_le(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_ge(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_cmp(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_hash(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_eq_timestamptz(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_ne_timestamptz(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_lt_timestamptz(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_gt_timestamptz(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_le_timestamptz(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_ge_timestamptz(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_cmp_timestamptz(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_eq_timestamp(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_ne_timestamp(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_lt_timestamp(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_gt_timestamp(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_le_timestamp(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_ge_timestamp(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_cmp_timestamp(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_eq(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_ne(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_lt(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_gt(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_le(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_ge(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_cmp(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_hash(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
overlaps_timestamp(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_smaller(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_larger(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_mi(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_justify_interval(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_justify_hours(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_justify_days(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_pl_interval(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_mi_interval(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_pl_interval(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_mi_interval(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_um(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_smaller(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_larger(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_pl(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_mi(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_mul(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
mul_d_interval(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_div(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_interval_div(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_interval_mod(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_accum(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_avg(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_decum(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_age(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_age(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_text(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
text_timestamp(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_text(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
text_timestamptz(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_text(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
text_interval(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_trunc(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_trunc(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_trunc(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

int 
isoweek2j(int year, int week)
{
	check_expected(year);
	check_expected(week);

	return (int) mock();
}

void 
isoweek2date(int woy, int *year, int *mon, int *mday)
{
	check_expected(woy);
	check_expected(year);
	check_expected(mon);
	check_expected(mday);
	optional_assignment(year);
	optional_assignment(mon);
	optional_assignment(mday);
	mock();
}

void 
isoweekdate2date(int isoweek, int isowday, int *year, int *mon, int *mday)
{
	check_expected(isoweek);
	check_expected(isowday);
	check_expected(year);
	check_expected(mon);
	check_expected(mday);
	optional_assignment(year);
	optional_assignment(mon);
	optional_assignment(mday);
	mock();
}

int 
date2isoweek(int year, int mon, int mday)
{
	check_expected(year);
	check_expected(mon);
	check_expected(mday);

	return (int) mock();
}

int 
date2isoyear(int year, int mon, int mday)
{
	check_expected(year);
	check_expected(mon);
	check_expected(mday);

	return (int) mock();
}

int 
date2isoyearday(int year, int mon, int mday)
{
	check_expected(year);
	check_expected(mon);
	check_expected(mday);

	return (int) mock();
}

Datum 
timestamp_part(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_part(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_part(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_zone(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_izone(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamp_timestamptz(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_timestamp(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_zone(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
timestamptz_izone(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
generate_series_timestamp(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
generate_series_timestamptz(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_amalg(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}

Datum 
interval_demalg(FunctionCallInfo fcinfo)
{
	check_expected(fcinfo);

	return (Datum) mock();
}
