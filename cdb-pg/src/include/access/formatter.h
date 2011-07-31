/*-------------------------------------------------------------------------
 *
 * access/formatter.h
 *	  Declarations for External Table Formatter functions
 *
 * Copyright (c) 2010, EMC corporation
 *-------------------------------------------------------------------------
 */

#ifndef FORMATTER_H
#define FORMATTER_H

#include "access/tupdesc.h"
#include "lib/stringinfo.h"
#include "nodes/nodes.h"
#include "nodes/value.h"
#include "utils/rel.h"

#define FMT_BADROW_MAXLEN 1024

typedef enum FmtNotification
{
	FMT_NONE,
	FMT_NEED_MORE_DATA,
	FMT_DATA_ERROR

} FmtNotification;

/*
 * FormatterData is the node type that is passed as fmgr "context" info
 * when a function is called by the External Table Formatter manager.
 */

typedef struct FormatterData
{
	NodeTag			type;                 /* see T_FormatterData */
	Relation    	fmt_relation;
	TupleDesc   	fmt_tupDesc;
	StringInfoData	fmt_databuf;
	bool			fmt_saw_eof;
	void		   *fmt_user_ctx;
	int				fmt_badrow_num;
	int				fmt_badrow_len;
	char			fmt_badrow_data[FMT_BADROW_MAXLEN];
	FmtNotification fmt_notification;
	List		   *fmt_args;
	FmgrInfo   	   *fmt_conv_funcs; /* in_fuctions (RET) or out_functions (WET) */
	MemoryContext	fmt_perrow_ctx;
	HeapTuple		fmt_tuple; /* hack! pass back the tuple through here for now... */
		
} FormatterData;

#define CALLED_AS_FORMATTER(fcinfo) \
	((fcinfo->context != NULL && IsA((fcinfo)->context, FormatterData)))

#define FORMATTER_GET_TUPDESC(fcinfo)         (((FormatterData*) fcinfo->context)->fmt_tupDesc)
#define FORMATTER_GET_RELATION(fcinfo)        (((FormatterData*) fcinfo->context)->fmt_relation)
#define FORMATTER_GET_DATABUF(fcinfo)         (((FormatterData*) fcinfo->context)->fmt_databuf.data)
#define FORMATTER_GET_DATALEN(fcinfo)         (((FormatterData*) fcinfo->context)->fmt_databuf.len)
#define FORMATTER_GET_DATACURSOR(fcinfo)      (((FormatterData*) fcinfo->context)->fmt_databuf.cursor)
#define FORMATTER_GET_SAW_EOF(fcinfo)	      (((FormatterData*) fcinfo->context)->fmt_saw_eof)
#define FORMATTER_GET_USER_CTX(fcinfo)        (((FormatterData*) fcinfo->context)->fmt_user_ctx)
#define FORMATTER_GET_PER_ROW_MEM_CTX(fcinfo) (((FormatterData*) fcinfo->context)->fmt_perrow_ctx)
#define FORMATTER_GET_CONVERSION_FUNCS(fcinfo) (((FormatterData*) fcinfo->context)->fmt_conv_funcs)
#define FORMATTER_GET_ARG_LIST(fcinfo)	      (((FormatterData*) fcinfo->context)->fmt_args)
#define FORMATTER_GET_NUM_ARGS(fcinfo)	      (list_length(FORMATTER_GET_ARG_LIST(fcinfo)))
#define FORMATTER_GET_NTH_ARG_KEY(fcinfo, n)  (((DefElem *)(list_nth(FORMATTER_GET_ARG_LIST(fcinfo),(n - 1))))->defname)
#define FORMATTER_GET_NTH_ARG_VAL(fcinfo, n)  (((Value *)((DefElem *)(list_nth(FORMATTER_GET_ARG_LIST(fcinfo),(n - 1))))->arg)->val.str)

#define FORMATTER_SET_USER_CTX(fcinfo, p) \
	(((FormatterData*) fcinfo->context)->fmt_user_ctx = p)

#define FORMATTER_SET_BAD_ROW_NUM(fcinfo, n) \
	(((FormatterData*) fcinfo->context)->fmt_badrow_num = n)

#define FORMATTER_SET_BAD_ROW_DATA(fcinfo, p, n) \
	(memcpy(((FormatterData*) fcinfo->context)->fmt_badrow_data, p, n))

#define FORMATTER_SET_DATACURSOR(fcinfo, n) \
	((((FormatterData*) fcinfo->context)->fmt_databuf.cursor) = n)

#define FORMATTER_RETURN_TUPLE(tuple) \
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple))
	
#define FORMATTER_RETURN_NOTIFICATION(fcinfo, n) \
	do { \
		(((FormatterData*) fcinfo->context)->fmt_notification = n); \
		PG_RETURN_NULL(); \
	} while (0)


#endif /* FORMATTER_H */
