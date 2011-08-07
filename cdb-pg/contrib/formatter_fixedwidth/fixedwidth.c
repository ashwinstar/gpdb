
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"

#include "access/formatter.h"
#include "catalog/pg_proc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/typcache.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "commands/copy.h"
#include <unistd.h>

/* Do the module magic dance */
PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(fixedwidth_out);
PG_FUNCTION_INFO_V1(fixedwidth_in);

Datum fixedwidth_out(PG_FUNCTION_ARGS);
Datum fixedwidth_in(PG_FUNCTION_ARGS);

typedef struct formatConfig
{
	/*
	 * Normaly we would have only one list of structs, each struct containing three fields:
	 * name, size, index. The reason we use three lists here is because we work with the infrastructure
	 * function CopyGetAttnums, which expects as input a list of names and returns a list of indexes.
	 * fldIndexes - holds the index of each field fetched from the file, into the fields description array
	 * tupdesc->attr[...]
	 */
	List       *fldNames;
	List       *fldSizes;
	List       *fldIndexes;
	List       *fldNullsWithBlanks;
	int         fields_tot_size;
	
	/*
	 * formating parameters
	 */
	int         preserve_blanks;
	char       *null_value;
	char       *line_delimiter;
	int         line_delimiter_length;
	
	/*
	 * infrastructure variables required by postgres "type resolution" methods
	 */
	FmgrInfo   *conv_functions;
	Oid        *typioparams;
	
} FormatConfig;

typedef struct {
	int            ncols;
	Datum         *values;
	bool          *nulls;
	int            buflen;
	bytea         *buffer;
	StringInfoData one_val;
	StringInfoData one_field;
} format_t;

static void 
init_format_t(format_t** data, int ncolumns, FunctionCallInfo fcinfo)
{
	*data          = palloc(sizeof(format_t));
	(*data)->ncols   = ncolumns;
	(*data)->values  = palloc(sizeof(Datum) * ncolumns);
	(*data)->nulls   = palloc(sizeof(bool) * ncolumns);
	initStringInfo( &((*data)->one_val) );
	initStringInfo( &((*data)->one_field) );
	
	FORMATTER_SET_USER_CTX(fcinfo, *data);	
	
}

static void
extract_field(char *data_cursor, int field_total_length, int preserve_blanks, StringInfo output)
{
	int actual_length, final_size;
	
	/*
	 * the actual length of the string we will restore into the database depends whether
	 * we preserve_blanks or not.
	 */
	if (0 != preserve_blanks)
	{
		actual_length = field_total_length;
	}
	else 
	{
		/*
		 * assume all field characters are blanks
		 */
		actual_length = 0;
		
		char *tail = data_cursor + field_total_length - 1;
		while ( tail != data_cursor)
		{
			if ( *tail != ' ' )
			{
				actual_length = tail - data_cursor + 1;
				break;
			}
			tail--;
		}
		
		if ( (tail == data_cursor) && (*data_cursor != ' ') )
		{
			actual_length  = 1;
		}
	}
	
	final_size = actual_length + 1;
	appendBinaryStringInfo(output, data_cursor, actual_length);
}

static void
reset_format_in_config(FormatConfig *format_config)
{
	format_config->preserve_blanks = 0;
	format_config->null_value = NULL;
	format_config->line_delimiter = "\n";
	format_config->line_delimiter_length = strlen(format_config->line_delimiter);
	format_config->fldNames = NIL;
	format_config->fldSizes = NIL;
	format_config->fldIndexes = NIL;
	format_config->fldNullsWithBlanks = NIL;
	format_config->fields_tot_size = 0;
}


static void
load_format_config(FormatConfig *format_config, FunctionCallInfo fcinfo)
{
	int   i;
	char *key;
	char *val;
	int   args_num = FORMATTER_GET_NUM_ARGS(fcinfo);
	
	reset_format_in_config(format_config);
	
	for ( i=1; i<=args_num; i++)
	{
		key = FORMATTER_GET_NTH_ARG_KEY(fcinfo, i);
		val = FORMATTER_GET_NTH_ARG_VAL(fcinfo, i);
		
		
		if ( strcasecmp("preserve_blanks", key) == 0)
		{
			if ( strcasecmp("on", val) == 0)
			{
				format_config->preserve_blanks = 1;
			}
		}
		else if ( strcasecmp("line_delim", key) == 0)
		{
			format_config->line_delimiter = val;
		}
		else if ( strcasecmp("null", key) == 0)
		{
			format_config->null_value = val;
		}
		else
		{
			int size = atoi(val);
			format_config->fldNames = lappend(format_config->fldNames, makeString(key));
			format_config->fldSizes = lappend_int(format_config->fldSizes, size);
			format_config->fields_tot_size += size;
		}		
	}
}

static char*
make_null_val_with_blanks(char *value, int field_size)
{
	char *ret;
	char *cur;
	int actual_size = field_size + 1;
	int size = strlen(value);
	
	if ( size > field_size)
	{
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("The size of the NULL value cannot be bigger than the field size")));
	}
	
	ret = (char*)palloc(actual_size);
	strcpy(ret, value);
	cur = ret + size;
	memset(cur, ' ', actual_size - size);
	ret[actual_size - 1] = '\0';
	
	return ret;
}

static char*
make_val_with_blanks(char *value, int field_size, StringInfo buf)
{
	int   sz;
	
	if (NULL != value )
	{
		sz = strlen(value);
		
		if ( sz > field_size)
		{
			ereport(ERROR,
					(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
					 errmsg("The size of the value cannot be bigger than the field size")));			
		}
		
		appendBinaryStringInfo(buf, value, sz);
		appendStringInfoFill(buf, field_size - sz, ' ');
	}
	else 
	{
		appendStringInfoFill(buf, field_size, ' ');
	}
	
	return buf->data;
}

static void
fill_null_with_blanks_list(FormatConfig *format_in_config)
{
	ListCell *curSize;
	int field_size;
	
	foreach(curSize, format_in_config->fldSizes)
	{
		field_size = lfirst_int(curSize);
		format_in_config->fldNullsWithBlanks = lappend(format_in_config->fldNullsWithBlanks, 
													   makeString(make_null_val_with_blanks(format_in_config->null_value, field_size)));
	}
}

static void
init_format_in_config(FormatConfig *format_in_config, int ncolumns, TupleDesc tupdesc, FunctionCallInfo fcinfo)
{
	load_format_config(format_in_config, fcinfo);
	
	if (format_in_config->null_value != NULL)
	{
		fill_null_with_blanks_list(format_in_config);
	}
	
	format_in_config->conv_functions = FORMATTER_GET_CONVERSION_FUNCS(fcinfo);
	format_in_config->typioparams = FORMATTER_GET_TYPIOPARAMS(fcinfo);	
	format_in_config->fldIndexes = CopyGetAttnums(tupdesc, FORMATTER_GET_RELATION(fcinfo), format_in_config->fldNames);	
}

static void
init_format_out_config(FormatConfig *format_out_config, int ncolumns, TupleDesc tupdesc, FunctionCallInfo fcinfo)
{
	load_format_config(format_out_config, fcinfo);
	
	if (format_out_config->null_value != NULL)
	{
		fill_null_with_blanks_list(format_out_config);
	}
	
	format_out_config->conv_functions = FORMATTER_GET_CONVERSION_FUNCS(fcinfo);	
	format_out_config->fldIndexes = CopyGetAttnums(tupdesc, FORMATTER_GET_RELATION(fcinfo), format_out_config->fldNames);	
}

static void 
get_tuple_info(TupleDesc tupdesc, int *r_ncolumns, format_t **r_myData, char **data, 
			   FunctionCallInfo fcinfo, FormatConfig *format_out_config)
{
	HeapTupleData		tuple;	
	HeapTupleHeader		rec	= PG_GETARG_HEAPTUPLEHEADER(0);	
	/* Get our internal description of the formatter */
	*r_ncolumns = tupdesc->natts;
	int ncolumns = *r_ncolumns;	
	*r_myData = (format_t *) FORMATTER_GET_USER_CTX(fcinfo);
	format_t *myData = *r_myData;
	
	if (myData == NULL)
	{
		myData          = palloc(sizeof(format_t));
		*r_myData       = myData;
		
		myData->ncols   = ncolumns;
		myData->values  = palloc(sizeof(Datum) * ncolumns);
		myData->nulls   = palloc(sizeof(bool) * ncolumns);
		initStringInfo( &(myData->one_field) );
		
		
		init_format_out_config(format_out_config, ncolumns, tupdesc, fcinfo);
		
		/* Determine required buffer size */
		myData->buflen = format_out_config->fields_tot_size + strlen(format_out_config->line_delimiter);
		myData->buflen = Max(128, myData->buflen);  /* allocate at least 128 bytes */
		myData->buffer = palloc(myData->buflen + VARHDRSZ);
		
		FORMATTER_SET_USER_CTX(fcinfo, myData);
	}
	if (myData->ncols != ncolumns)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("formatter_export: unexpected change of output record type")));	
	
	/* break the input tuple into fields */
	tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_data = rec;
	heap_deform_tuple(&tuple, tupdesc, myData->values, myData->nulls);
	*data = VARDATA(myData->buffer);
	
}

Datum 
fixedwidth_out(PG_FUNCTION_ARGS)
{
	TupleDesc           tupdesc;
	MemoryContext 		m, oldcontext;
	int                 ncolumns = 0;
	format_t           *myData;
	char               *data;
	int                 datlen = 0;
	ListCell           *curIdx; 
	ListCell           *curSize;
	int                 field_size;
	char               *mapped_val;
	char               *mapped_val_with_blanks;
	bool		        isnull;
	Datum		        value;
	int			        idx;
	static FormatConfig format_out_config;
	
	/* Must be called via the external table format manager */
	if (!CALLED_AS_FORMATTER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("fixedwidth_out: not called by format manager")));	
	
	
	tupdesc = FORMATTER_GET_TUPDESC(fcinfo);
	get_tuple_info(tupdesc, &ncolumns, &myData, &data, fcinfo, &format_out_config);
	
	
	
	/* =======================================================================
	 *                            MAIN FORMATTING CODE
	 * ======================================================================= */
	m = FORMATTER_GET_PER_ROW_MEM_CTX(fcinfo); 
	oldcontext = MemoryContextSwitchTo(m); 
	
	forboth(curIdx, format_out_config.fldIndexes, curSize, format_out_config.fldSizes)
	{
		field_size = lfirst_int(curSize);
		idx  = lfirst_int(curIdx) - 1;		
		isnull = myData->nulls[idx];
		value = myData->values[idx];
		
		resetStringInfo(&(myData->one_field));
		
		if ( isnull )
		{
			mapped_val_with_blanks = make_val_with_blanks(format_out_config.null_value, field_size, &(myData->one_field));
		}
		else 
		{
			mapped_val = OutputFunctionCall(&format_out_config.conv_functions[idx], value);
			mapped_val_with_blanks = make_val_with_blanks(mapped_val, field_size, &(myData->one_field));
		}

		memcpy(&data[datlen], mapped_val_with_blanks, field_size);
		datlen += field_size;
	}
	
	memcpy(&data[datlen], format_out_config.line_delimiter, format_out_config.line_delimiter_length);
	datlen += format_out_config.line_delimiter_length;
	
	MemoryContextSwitchTo(oldcontext);
	/* ======================================================================= */
	
	SET_VARSIZE(myData->buffer, datlen + VARHDRSZ);
		
	PG_RETURN_BYTEA_P(myData->buffer);
}

/*
 * fixedwidth_in
 * each time this function is called, it builds one tuple from the input data buffer
 */

Datum 
fixedwidth_in(PG_FUNCTION_ARGS)
{	
	HeapTuple			tuple;
	TupleDesc           tupdesc;
	MemoryContext 		m, oldcontext;
	format_t           *myData;
	char               *data_buf;
	int                 ncolumns = 0;
	int			  		data_cur;
	int                 data_len;
	bool                saw_eof;	
	ListCell           *curIdx; 
	ListCell           *curSize;
	ListCell           *cur_null_with_blanks;	
	int		            remaining;
	int                 field_size;
	char               *nullval;
	int			        idx;
	char               *null_val_with_blanks;
	static FormatConfig format_in_config;
	
	/* Must be called via the external table format manager */
	if (!CALLED_AS_FORMATTER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("fixedwidth_in: not called by format manager")));
		
	tupdesc = FORMATTER_GET_TUPDESC(fcinfo);
	
	/* Get our internal description of the formatter */
	ncolumns = tupdesc->natts;	
	myData = (format_t *) FORMATTER_GET_USER_CTX(fcinfo);
	
	if (myData == NULL)
	{
		init_format_in_config(&format_in_config, ncolumns, tupdesc, fcinfo);
		init_format_t(&myData, ncolumns, fcinfo);			
	}
	
	/* start clean */
	MemSet(myData->values, 0, ncolumns * sizeof(Datum));
	MemSet(myData->nulls, true, ncolumns * sizeof(bool));

	/* get our input data buf and number of valid bytes in it */
	data_buf = FORMATTER_GET_DATABUF(fcinfo);
	data_len = FORMATTER_GET_DATALEN(fcinfo); 
	data_cur = FORMATTER_GET_DATACURSOR(fcinfo);
	saw_eof  = FORMATTER_GET_SAW_EOF(fcinfo);
	
	
	/* =======================================================================
	 *                            MAIN FORMATTING CODE
	 * ======================================================================= */	
	/*
	 * tuple data extraction is done in a separate memory context
	 */
	m = FORMATTER_GET_PER_ROW_MEM_CTX(fcinfo); 
	oldcontext = MemoryContextSwitchTo(m); 
			
	if (format_in_config.null_value != NULL)
		cur_null_with_blanks = list_head(format_in_config.fldNullsWithBlanks);
	
	forboth(curIdx, format_in_config.fldIndexes, curSize, format_in_config.fldSizes)
	{
		remaining	= 0;
		field_size = lfirst_int(curSize);
		nullval = format_in_config.null_value;		
		remaining = data_len - data_cur;	
		
		if (remaining < field_size)
		{
			if (  saw_eof && remaining )
			{
				data_cur += remaining;
				FORMATTER_SET_DATACURSOR(fcinfo, data_cur);
				ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
						 errmsg("Last line in the file contains an incomplete tuple")));
				
			}
			else 
			{
				MemoryContextSwitchTo(oldcontext);
				FORMATTER_RETURN_NOTIFICATION(fcinfo, FMT_NEED_MORE_DATA);				
			}

		}
		
		resetStringInfo(&(myData->one_val));
		
		idx  = lfirst_int(curIdx) - 1;

		if ( 0 == format_in_config.preserve_blanks )
		/*
		 * trailing blanks are removed from the field value
		 */
		{
			extract_field(data_buf + data_cur, field_size, 0, &(myData->one_val));
			/*
			 * there are two (2) cases when we set value to null:
			 * a. there is a null value defined in the formatter arguments, and this value was found in the field
			 * b. there is no null value defined and the field contained only blanks
			 */ 
			if ( !( (nullval != NULL) && (strcmp(myData->one_val.data, nullval) == 0) )  &&
				 /* we are not in case 1 */
				 !( (nullval == NULL) && (strcmp(myData->one_val.data, "\0") == 0) ) )
				 /* and also not in case 2 */
			{			
				myData->values[idx] = InputFunctionCall(&format_in_config.conv_functions[idx],
											  myData->one_val.data,
											  format_in_config.typioparams[idx],
											  tupdesc->attrs[idx]->atttypmod);
				myData->nulls[idx] = false;
			}
		}
		else 
		/*
		 * trailing blanks are are kept in the field value
		 */			
		{
			if (nullval == NULL)
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("A null_value was not defined. When preserve_blanks is on, a null_value \
								 must be defined in the formatter arguments string")));
			}
			
			extract_field(data_buf + data_cur, field_size, 1, &(myData->one_val));
			
			null_val_with_blanks = strVal(lfirst(cur_null_with_blanks));
			cur_null_with_blanks = lnext(cur_null_with_blanks);
			
			if (strcmp(myData->one_val.data, null_val_with_blanks) != 0)
			{
				myData->values[idx] = InputFunctionCall(&format_in_config.conv_functions[idx],
													  myData->one_val.data,
													  format_in_config.typioparams[idx],
													  tupdesc->attrs[idx]->atttypmod);
				myData->nulls[idx] = false;
			}
		}
		data_cur += field_size;
	}	
	
	/*
	 * go over the line delimiter
	 */
	data_cur += format_in_config.line_delimiter_length;	

	/*
	 * wrapping up
	 */
	MemoryContextSwitchTo(oldcontext);
	/* ======================================================================= */


	FORMATTER_SET_DATACURSOR(fcinfo, data_cur);

	tuple = heap_form_tuple(tupdesc, myData->values, myData->nulls);

	/* hack... pass tuple here. don't free prev tuple - the executor does it  */
	((FormatterData*) fcinfo->context)->fmt_tuple = tuple;
	
	FORMATTER_RETURN_TUPLE(tuple);
}
