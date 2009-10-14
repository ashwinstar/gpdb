/*-------------------------------------------------------------------------
 *
 * xlogloc.c
 *
 * Copyright (c) 2007-2009, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "access/xlog.h"

#include "access/heapam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "parser/parsetree.h"
#include "utils/acl.h"
#include "utils/builtins.h"

#define DatumGetXLogLoc(X)	     ((XLogRecPtr*) DatumGetPointer(X))
#define XLogLocGetDatum(X)	     PointerGetDatum(X)
#define PG_GETARG_XLOGLOC(n)     DatumGetXLogLoc(PG_GETARG_DATUM(n))
#define PG_RETURN_XLOGLOC(x)     return XLogLocGetDatum(x)

#define LDELIM			'('
#define RDELIM			')'
#define DELIM			'/'
#define NXLOGLOCARGS	2

/* ----------------------------------------------------------------
 *		gpxloglocin
 * ----------------------------------------------------------------
 */
Datum
gpxloglocin(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	char	   *p,
			   *coord[NXLOGLOCARGS];
	int			i;
	XLogRecPtr	*result;
	uint32		result_xlogid;
	uint32		result_xrecoff;
	char	   *badp;

	for (i = 0, p = str; *p && i < NXLOGLOCARGS && *p != RDELIM; p++)
		if (*p == DELIM || (*p == LDELIM && !i))
			coord[i++] = p + 1;

	if (i < NXLOGLOCARGS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type gpxlogloc: \"%s\"",
						str)));

	errno = 0;
	result_xlogid = strtoul(coord[0], &badp, 16);	// Hexadecimal.
	if (errno || *badp != DELIM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type gpxlogloc: \"%s\"",
						str)));

	result_xrecoff = strtoul(coord[1], &badp, 16);	// Hexadecimal.
	if (errno || *badp != RDELIM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type gpxlogloc: \"%s\"",
						str)));

	result = (XLogRecPtr*) palloc(sizeof(XLogRecPtr));

	result->xlogid = result_xlogid;
	result->xrecoff = result_xrecoff;

	PG_RETURN_XLOGLOC(result);
}

/* ----------------------------------------------------------------
 *		gpxloglocout
 * ----------------------------------------------------------------
 */
Datum
gpxloglocout(PG_FUNCTION_ARGS)
{
	XLogRecPtr *xlogLoc = PG_GETARG_XLOGLOC(0);
	char		buf[32];

	snprintf(buf, sizeof(buf), "(%X/%X)", xlogLoc->xlogid, xlogLoc->xrecoff);

	PG_RETURN_CSTRING(pstrdup(buf));
}

/*
 *		gpxloglocrecv			- converts external binary format to xlog location
 */
Datum
gpxloglocrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	XLogRecPtr	*result;
	uint32		result_xlogid;
	uint32		result_xrecoff;

	result_xlogid = pq_getmsgint(buf, sizeof(result_xlogid));
	result_xrecoff = pq_getmsgint(buf, sizeof(result_xrecoff));

	result = (XLogRecPtr*) palloc(sizeof(XLogRecPtr));

	result->xlogid = result_xlogid;
	result->xrecoff = result_xrecoff;

	PG_RETURN_XLOGLOC(result);
}

/*
 *		gpxloglocsend			- converts xlog location to binary format
 */
Datum
gpxloglocsend(PG_FUNCTION_ARGS)
{
	XLogRecPtr *xlogLoc = PG_GETARG_XLOGLOC(0);
	uint32		send_xlogid;
	uint32		send_xrecoff;
	StringInfoData buf;

	send_xlogid = xlogLoc->xlogid;
	send_xrecoff = xlogLoc->xrecoff;

	pq_begintypsend(&buf);
	pq_sendint(&buf, send_xlogid, sizeof(send_xlogid));
	pq_sendint(&buf, send_xrecoff, sizeof(send_xrecoff));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

