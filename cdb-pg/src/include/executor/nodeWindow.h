/*-------------------------------------------------------------------------
 *
 * nodeWindow.h
 *	  prototypes for nodeWindow.c
 *
 *
 * Copyright (c) 2007, greenplum inc *
 *
 * IDENTIFICATION
 *	  $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEWINDOW_H
#define NODEWINDOW_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsWindow(Window *node);
extern WindowState *ExecInitWindow(Window *node, EState *estate, int eflags);
extern TupleTableSlot *ExecWindow(WindowState *node);
extern void ExecEndWindow(WindowState *node);
extern void ExecReScanWindow(WindowState *node, ExprContext *exprCtxt);

/* Dummy window function implementation for pg_proc (prosrc). */
extern Datum window_dummy(PG_FUNCTION_ARGS);

/* Internal-use function to tag tuples with originating segment. */
extern Datum mpp_execution_segment(PG_FUNCTION_ARGS);

/* Declarations for window function implementations. */
extern Datum row_number_immed(PG_FUNCTION_ARGS);
extern Datum rank_immed(PG_FUNCTION_ARGS);
extern Datum dense_rank_immed(PG_FUNCTION_ARGS);
extern Datum cume_dist_prelim(PG_FUNCTION_ARGS);
extern Datum percent_rank_final(PG_FUNCTION_ARGS);
extern Datum cume_dist_final(PG_FUNCTION_ARGS);
Datum ntile_prelim_int(PG_FUNCTION_ARGS);
Datum ntile_prelim_bigint(PG_FUNCTION_ARGS);
Datum ntile_prelim_numeric(PG_FUNCTION_ARGS);
Datum ntile_final(PG_FUNCTION_ARGS);
extern Datum lead_int(PG_FUNCTION_ARGS);
extern Datum lead_bool(PG_FUNCTION_ARGS);
extern Datum lead_char(PG_FUNCTION_ARGS);
extern Datum lead_cidr(PG_FUNCTION_ARGS);
extern Datum lead_circle(PG_FUNCTION_ARGS);
extern Datum lead_float4(PG_FUNCTION_ARGS);
extern Datum lead_float8(PG_FUNCTION_ARGS);
extern Datum lead_inet(PG_FUNCTION_ARGS);
extern Datum lead_interval(PG_FUNCTION_ARGS);
extern Datum lead_line(PG_FUNCTION_ARGS);
extern Datum lead_lseg(PG_FUNCTION_ARGS);
extern Datum lead_macaddr(PG_FUNCTION_ARGS);
extern Datum lead_smallint(PG_FUNCTION_ARGS);
extern Datum lead_int8(PG_FUNCTION_ARGS);
extern Datum lead_money(PG_FUNCTION_ARGS);
extern Datum lead_name(PG_FUNCTION_ARGS);
extern Datum lead_numeric(PG_FUNCTION_ARGS);
extern Datum lead_oid(PG_FUNCTION_ARGS);
extern Datum lead_path(PG_FUNCTION_ARGS);
extern Datum lead_point(PG_FUNCTION_ARGS);
extern Datum lead_polygon(PG_FUNCTION_ARGS);
extern Datum lead_reltime(PG_FUNCTION_ARGS);
extern Datum lead_text(PG_FUNCTION_ARGS);
extern Datum lead_tid(PG_FUNCTION_ARGS);
extern Datum lead_time(PG_FUNCTION_ARGS);
extern Datum lead_timestamp(PG_FUNCTION_ARGS);
extern Datum lead_timestamptz(PG_FUNCTION_ARGS);
extern Datum lead_timetz(PG_FUNCTION_ARGS);
extern Datum lead_varbit(PG_FUNCTION_ARGS);
extern Datum lead_varchar(PG_FUNCTION_ARGS);
extern Datum lead_xid(PG_FUNCTION_ARGS);
extern Datum lead_any(PG_FUNCTION_ARGS);
extern Datum lead_bytea(PG_FUNCTION_ARGS);
extern Datum lead_box(PG_FUNCTION_ARGS);
extern Datum lead_bit(PG_FUNCTION_ARGS);
extern Datum lead_generic(PG_FUNCTION_ARGS);
extern Datum last_value_int(PG_FUNCTION_ARGS);
extern Datum last_value_bigint(PG_FUNCTION_ARGS);
extern Datum last_value_smallint(PG_FUNCTION_ARGS);
extern Datum last_value_numeric(PG_FUNCTION_ARGS);
extern Datum last_value_text(PG_FUNCTION_ARGS);
extern Datum last_value_bool(PG_FUNCTION_ARGS);
extern Datum last_value_char(PG_FUNCTION_ARGS);
extern Datum last_value_cidr(PG_FUNCTION_ARGS);
extern Datum last_value_circle(PG_FUNCTION_ARGS);
extern Datum last_value_float4(PG_FUNCTION_ARGS);
extern Datum last_value_float8(PG_FUNCTION_ARGS);
extern Datum last_value_inet(PG_FUNCTION_ARGS);
extern Datum last_value_interval(PG_FUNCTION_ARGS);
extern Datum last_value_line(PG_FUNCTION_ARGS);
extern Datum last_value_lseg(PG_FUNCTION_ARGS);
extern Datum last_value_macaddr(PG_FUNCTION_ARGS);
extern Datum last_value_money(PG_FUNCTION_ARGS);
extern Datum last_value_name(PG_FUNCTION_ARGS);
extern Datum last_value_oid(PG_FUNCTION_ARGS);
extern Datum last_value_path(PG_FUNCTION_ARGS);
extern Datum last_value_point(PG_FUNCTION_ARGS);
extern Datum last_value_polygon(PG_FUNCTION_ARGS);
extern Datum last_value_reltime(PG_FUNCTION_ARGS);
extern Datum last_value_tid(PG_FUNCTION_ARGS);
extern Datum last_value_time(PG_FUNCTION_ARGS);
extern Datum last_value_timestamp(PG_FUNCTION_ARGS);
extern Datum last_value_timestamptz(PG_FUNCTION_ARGS);
extern Datum last_value_timetz(PG_FUNCTION_ARGS);
extern Datum last_value_varbit(PG_FUNCTION_ARGS);
extern Datum last_value_varchar(PG_FUNCTION_ARGS);
extern Datum last_value_xid(PG_FUNCTION_ARGS);
extern Datum last_value_generic(PG_FUNCTION_ARGS);
extern Datum first_value_bool(PG_FUNCTION_ARGS);
extern Datum first_value_char(PG_FUNCTION_ARGS);
extern Datum first_value_cidr(PG_FUNCTION_ARGS);
extern Datum first_value_circle(PG_FUNCTION_ARGS);
extern Datum first_value_float4(PG_FUNCTION_ARGS);
extern Datum first_value_float8(PG_FUNCTION_ARGS);
extern Datum first_value_inet(PG_FUNCTION_ARGS);
extern Datum first_value_interval(PG_FUNCTION_ARGS);
extern Datum first_value_line(PG_FUNCTION_ARGS);
extern Datum first_value_lseg(PG_FUNCTION_ARGS);
extern Datum first_value_macaddr(PG_FUNCTION_ARGS);
extern Datum first_value_smallint(PG_FUNCTION_ARGS);
extern Datum first_value_int4(PG_FUNCTION_ARGS);
extern Datum first_value_int8(PG_FUNCTION_ARGS);
extern Datum first_value_money(PG_FUNCTION_ARGS);
extern Datum first_value_name(PG_FUNCTION_ARGS);
extern Datum first_value_numeric(PG_FUNCTION_ARGS);
extern Datum first_value_oid(PG_FUNCTION_ARGS);
extern Datum first_value_path(PG_FUNCTION_ARGS);
extern Datum first_value_point(PG_FUNCTION_ARGS);
extern Datum first_value_polygon(PG_FUNCTION_ARGS);
extern Datum first_value_reltime(PG_FUNCTION_ARGS);
extern Datum first_value_text(PG_FUNCTION_ARGS);
extern Datum first_value_tid(PG_FUNCTION_ARGS);
extern Datum first_value_time(PG_FUNCTION_ARGS);
extern Datum first_value_timestamp(PG_FUNCTION_ARGS);
extern Datum first_value_timestamptz(PG_FUNCTION_ARGS);
extern Datum first_value_timetz(PG_FUNCTION_ARGS);
extern Datum first_value_varbit(PG_FUNCTION_ARGS);
extern Datum first_value_varchar(PG_FUNCTION_ARGS);
extern Datum first_value_xid(PG_FUNCTION_ARGS);
extern Datum first_value_generic(PG_FUNCTION_ARGS);
extern Datum lag_bool(PG_FUNCTION_ARGS);
extern Datum lag_char(PG_FUNCTION_ARGS);
extern Datum lag_cidr(PG_FUNCTION_ARGS);
extern Datum lag_circle(PG_FUNCTION_ARGS);
extern Datum lag_float4(PG_FUNCTION_ARGS);
extern Datum lag_float8(PG_FUNCTION_ARGS);
extern Datum lag_inet(PG_FUNCTION_ARGS);
extern Datum lag_interval(PG_FUNCTION_ARGS);
extern Datum lag_line(PG_FUNCTION_ARGS);
extern Datum lag_lseg(PG_FUNCTION_ARGS);
extern Datum lag_macaddr(PG_FUNCTION_ARGS);
extern Datum lag_smallint(PG_FUNCTION_ARGS);
extern Datum lag_int4(PG_FUNCTION_ARGS);
extern Datum lag_int8(PG_FUNCTION_ARGS);
extern Datum lag_money(PG_FUNCTION_ARGS);
extern Datum lag_name(PG_FUNCTION_ARGS);
extern Datum lag_numeric(PG_FUNCTION_ARGS);
extern Datum lag_oid(PG_FUNCTION_ARGS);
extern Datum lag_path(PG_FUNCTION_ARGS);
extern Datum lag_point(PG_FUNCTION_ARGS);
extern Datum lag_polygon(PG_FUNCTION_ARGS);
extern Datum lag_reltime(PG_FUNCTION_ARGS);
extern Datum lag_text(PG_FUNCTION_ARGS);
extern Datum lag_tid(PG_FUNCTION_ARGS);
extern Datum lag_time(PG_FUNCTION_ARGS);
extern Datum lag_timestamp(PG_FUNCTION_ARGS);
extern Datum lag_timestamptz(PG_FUNCTION_ARGS);
extern Datum lag_timetz(PG_FUNCTION_ARGS);
extern Datum lag_varbit(PG_FUNCTION_ARGS);
extern Datum lag_varchar(PG_FUNCTION_ARGS);
extern Datum lag_xid(PG_FUNCTION_ARGS);
extern Datum lag_any(PG_FUNCTION_ARGS);
extern Datum lag_bytea(PG_FUNCTION_ARGS);
extern Datum lag_box(PG_FUNCTION_ARGS);
extern Datum lag_bit(PG_FUNCTION_ARGS);
extern Datum lag_generic(PG_FUNCTION_ARGS);
#endif   /* NODEWINDOW_H */
