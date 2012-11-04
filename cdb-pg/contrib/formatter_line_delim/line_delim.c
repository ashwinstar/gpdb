/*
 *  line_delim.c - An example for custom format for both input and output.
 *  This example allows the user to define a custom line delimiter through the sql file.
 *  It also allows a line delimiter larger than one character.
 *
 *  This file can be used as a base for writing User Defined Formats.
 */

#include <stdio.h>

#include"postgres.h"
#include"fmgr.h"
#include"funcapi.h"

#include"access/formatter.h"
#include"catalog/pg_proc.h"
#include"utils/builtins.h"
#include"utils/memutils.h"
#include"utils/typcache.h"
#include"utils/syscache.h"

/*
 * Maximum size string to support, affects allocation size of the tuple buffer.
 * Only used for variable length strings.  For strings with a declared typmod
 * we allow that size even if it is larger than this.
 */
#define MAX_FORMAT_STRING 4096

/* Maximum digits in numbers */
#define MAX_DIGIT_LEN 30

/* Do the module magic dance */
PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(formatter_import);
PG_FUNCTION_INFO_V1(formatter_export);

typedef struct {
	int			ncols;				/* number of columns in the table */
	Datum		*values;			/* record value */
	bool		*nulls;				/* true if the current record is nulll. False otherwise */
	int			buflen;
	bytea		*buffer;
} format_t;

typedef struct formatConfig
{
	/* formating parameters */
	char		*line_delimiter;
	int         line_delimiter_length;
	char		entry_delimiter;

	/* infrastructure variables required by postgres "type resolution" methods */
	FmgrInfo	*conv_functions;	/* conv_functions[i] points to a function that converts column i from GPDB types to C types and vice versa */
	Oid			*typioparams;		/* typioparams[i] - the type in column i */

} format_config_t;

/*
 * receives cur_line and delim,
 * REPLACES the next delimiter with  '\0'
 * returns a pointer to the beginning of the next entry
 */
static char* get_next_token(char *cur_line, char delim)
{
	char *next_entry;

	if (cur_line)
		next_entry = strchr(cur_line, delim);
	else
		return NULL;

	/* at the end of the line next_entry is null, this is not an error. */
	if (next_entry)
	{
		next_entry[0] = '\0';
		next_entry++;
	}
	return next_entry;
}

/*
 * load_format_config
 *
 * parse the user specified fixed width keywords. Currently supported
 * keywords are: 'line_delim' and 'entry_delim'. any other
 * unrecognized keywords are ignored.
 */
static void load_format_config(format_config_t *format_config, FunctionCallInfo fcinfo)
{
	int		i;
	char	*key;
	char	*val;
	int		args_num = FORMATTER_GET_NUM_ARGS(fcinfo);

	/* set default */
	format_config->line_delimiter = "\n";
	format_config->line_delimiter_length = 1;
	format_config->entry_delimiter = ' ';

	for (i = 1; i <= args_num; i++)
	{
		key = FORMATTER_GET_NTH_ARG_KEY(fcinfo, i);
		val = FORMATTER_GET_NTH_ARG_VAL(fcinfo, i);

		if ( strcasecmp("line_delim", key) == 0)
		{
			format_config->line_delimiter = val;
			format_config->line_delimiter_length = strlen(format_config->line_delimiter);
		}
		else if ( strcasecmp("entry_delim", key) == 0)
		{
			if (strlen(val) > 1)
				elog(ERROR, "column delimiter must be one character\n");
			else
				format_config->entry_delimiter = val[0];
		}
	}
}

/*
 * Initializes CTX for this transaction.
 * Should be called only once for every segment.
 * it is the users responsibility to call this function only when the CTX is not initialized.
 */
static void init_export(PG_FUNCTION_ARGS)
{
	format_t*		my_data;			/* the object to be initialized. The user will get it later by calling FORMATTER_GET_USER_CTX(fcinfo); */
	int				ncolumns;			/* number of columns in table. */
	TupleDesc		tupdesc;
	int				i;

	tupdesc = FORMATTER_GET_TUPDESC(fcinfo);
	ncolumns = tupdesc->natts;

	/* Initialize memory: */
	my_data = palloc(sizeof(format_t));
	my_data->ncols = ncolumns;
	my_data->values = palloc(sizeof(Datum) * ncolumns);
	my_data->nulls = palloc(sizeof(bool) * ncolumns);

	my_data->buflen = 0;
	/* Determine required buffer size by looping over columns and determine how much memory is needed */
	for (i = 0; i < ncolumns; i++)
	{
		Oid   type = tupdesc->attrs[i]->atttypid;
		int32 typmod = tupdesc->attrs[i]->atttypmod;
		switch (type)
		{
		case FLOAT4OID:
		case FLOAT8OID:
			my_data->buflen += sizeof(double);
			break;

		case VARCHAROID:
		case BPCHAROID:
		case TEXTOID:
			my_data->buflen += (typmod > 0) ? typmod : MAX_FORMAT_STRING;
			break;

			/*
			 * --------------------
			 * add other types here
			 * --------------------
			 */
		default:
			elog(ERROR, "The line_delim formatter does not yet support type %d", (int) type);
			break;
		}
	}
	my_data->buflen = Max(128, my_data->buflen);  /* allocate at least 128 bytes */
	my_data->buffer = palloc(my_data->buflen + VARHDRSZ);

	FORMATTER_SET_USER_CTX(fcinfo , my_data);
}

/*
 *  The function for exporting data.
 *  To create custom formatting, few changes are needed to be made here,
 *  most of the structure is standard.
 */
Datum formatter_export(PG_FUNCTION_ARGS)
{
	HeapTupleHeader		 rec = PG_GETARG_HEAPTUPLEHEADER(0);
	TupleDesc			 tupdesc;
	HeapTupleData		 tuple;
	int					 ncolumns = 0;							/* number of columns in table */
	format_t			*my_data;								/* hold data and meta-data about current row */
	char				*data;									/* pointer to myData->buffer, but in char* instead of bytea */
	int					 datlen;
	int					 i;
	format_config_t		 format_config;

	/* Must be called via the external table format manager */
	if (!CALLED_AS_FORMATTER(fcinfo))
		elog(ERROR, "formatter_export: not called by format manager");

	/* load formatting configuration, such as line delimiter etc. */
	load_format_config(&format_config, fcinfo);

	tupdesc = FORMATTER_GET_TUPDESC(fcinfo);

	ncolumns = tupdesc->natts;

	/* Initialize memory if needed */
	my_data = (format_t *) FORMATTER_GET_USER_CTX(fcinfo);
	if (NULL == my_data )
	{
		init_export(fcinfo);
		my_data = (format_t *) FORMATTER_GET_USER_CTX(fcinfo);
		format_config.conv_functions = FORMATTER_GET_CONVERSION_FUNCS(fcinfo);
	}

	/* a change occurred from last call. */
	if (my_data->ncols != ncolumns)
		elog(ERROR, "formatter_export: unexpected change of output record type");

	/* break the input tuple into fields */
	tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
	tuple.t_data = rec;
	heap_deform_tuple(&tuple, tupdesc, my_data->values, my_data->nulls);

	/* initialize memory for output */
	datlen = 0;
	data = VARDATA(my_data->buffer);

	/* loop through columns */
	for (i = 0; i < ncolumns; i++)
	{
		char* cur_entry = OutputFunctionCall(&format_config.conv_functions[i], my_data->values[i]);

		/* ------------------------------------------------
		 * cur_entry now holds the data from the table.
		 * Custom formatting can be done here on cur_entry.
		 * ------------------------------------------------
		 */

		memcpy(&data[datlen], cur_entry, strlen(cur_entry));
		datlen += strlen(cur_entry);

		/* insert column delimiter, if not in last column */
		if (i != ncolumns - 1)
		{
			data[datlen] = format_config.entry_delimiter;
			datlen++;
		}
	}

	/* insert line delimiter */
	strcpy(&data[datlen], format_config.line_delimiter);
	datlen+=format_config.line_delimiter_length;

	SET_VARSIZE(my_data->buffer, datlen + VARHDRSZ);
	PG_RETURN_BYTEA_P(my_data->buffer);
}

/*
 *  The function for importing data.
 *  To create custom formatting, few changes are needed to be made here,
 *  most of the structure is standard.
 */
Datum formatter_import(PG_FUNCTION_ARGS)
{
	static format_config_t		format_config;

	HeapTuple			tuple;
	TupleDesc			tupdesc;
	MemoryContext		m, oldcontext;
	format_t			*my_data;
	char				*data_buf;
	int					ncolumns = 0;
	int					data_cur;
	int					data_len;
	int					linelen = 0;
	int					i;

	bool				saw_eof;					/* true if context saw eof. This doesn't mean we are in the last line! */
	bool				is_last_line = false;		/* true if the current line is that last line */

	char				*findnl;
	char				*cur_line;
	char				*cur_entry;
	char				*next_entry;

	/* Must be called via the external table format manager */
	if (!CALLED_AS_FORMATTER(fcinfo))
		elog(ERROR, "formatter_import: not called by format manager");

	tupdesc = FORMATTER_GET_TUPDESC(fcinfo);

	/* Get our internal description of the formatter */
	ncolumns = tupdesc->natts;

	my_data = (format_t *) FORMATTER_GET_USER_CTX(fcinfo);

	if (my_data == NULL) 						/* should enter here only once for each segment. */
	{
		elog(INFO, "My data is NULL");
		my_data = palloc(sizeof(format_t));
		my_data->ncols = ncolumns;
		my_data->values = palloc(sizeof(Datum) * ncolumns);
		my_data->nulls = palloc(sizeof(bool) * ncolumns);
		FORMATTER_SET_USER_CTX(fcinfo, my_data);
		format_config.conv_functions = FORMATTER_GET_CONVERSION_FUNCS(fcinfo);	/* get functions to convert from sql format to C format */
		format_config.typioparams = FORMATTER_GET_TYPIOPARAMS(fcinfo);
		/* load formatting configuration, such as line delimiter etc. */
		load_format_config(&format_config, fcinfo);
	}
	if (my_data->ncols != ncolumns)
		elog(ERROR, "formatter_import: unexpected change of output record type");

	/* get our input data buf and number of valid bytes in it */
	data_buf = FORMATTER_GET_DATABUF(fcinfo);
	data_len = FORMATTER_GET_DATALEN(fcinfo);
	data_cur = FORMATTER_GET_DATACURSOR(fcinfo);
	saw_eof  = FORMATTER_GET_SAW_EOF(fcinfo);

	/* start clean */
	MemSet(my_data->values, 0, ncolumns * sizeof(Datum));
	MemSet(my_data->nulls, true, ncolumns * sizeof(bool));

	m = FORMATTER_GET_PER_ROW_MEM_CTX(fcinfo);
	oldcontext = MemoryContextSwitchTo(m);

	if (data_cur >= data_len)
	{
		MemoryContextSwitchTo(oldcontext);
		FORMATTER_RETURN_NOTIFICATION(fcinfo, FMT_NEED_MORE_DATA);
	}

	/* find end of line or end of file */
	findnl = strstr((char*)data_buf + data_cur, format_config.line_delimiter);
	if (saw_eof && !findnl)
	{
			findnl = data_buf + data_len;		/* line ends in end of file */
			is_last_line = true;
	}
	else if (!findnl && !saw_eof)		/* didn't see EOL nor EOF. Need more data to read a full line */
	{
		MemoryContextSwitchTo(oldcontext);
		FORMATTER_RETURN_NOTIFICATION(fcinfo, FMT_NEED_MORE_DATA);
	}

	linelen = findnl - (char*)data_buf - data_cur;
	cur_line = palloc(linelen+1); /* leave space for '\0' at the end of the string */
	strncpy(cur_line, data_buf + data_cur, linelen);
	cur_line[linelen] = '\0';
	cur_entry = cur_line; /* beginning of the current line */

	/* loop over all columns in current row */
	for (i = 0; i < ncolumns; i++)
	{
		/* set the changed text */
		next_entry = get_next_token(cur_entry, format_config.entry_delimiter);

		if (NULL == next_entry && i != ncolumns -1)
		{
			int total_actual_field_size = linelen + format_config.line_delimiter_length;
			FORMATTER_SET_BAD_ROW_DATA(fcinfo, data_buf + data_cur, total_actual_field_size);
			FORMATTER_SET_BYTE_NUMBER(fcinfo, total_actual_field_size);

			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("not enough columns on one row")));
		}
		/* -------------------------------------------------------
		 * cur_entry now holds the raw data as read from the file.
		 * Custom formatting can be done here on cur_entry.
		 * -------------------------------------------------------
		 */

		my_data->values[i] = InputFunctionCall(&format_config.conv_functions[i],
									  cur_entry,
									  format_config.typioparams[i],
									  tupdesc->attrs[i]->atttypmod);
		my_data->nulls[i] = false;

		/* continue to next entry */
		cur_entry = next_entry;
 	}

	/* If there is more to read on this line, omit error message
	 * If LOG ERRORS clause is defined, the error will go into the error table, and the transacation will continue
	 */
	if (NULL != next_entry)
	{
		int total_actual_field_size = linelen + format_config.line_delimiter_length;
		FORMATTER_SET_BAD_ROW_DATA(fcinfo, data_buf + data_cur, total_actual_field_size);
		FORMATTER_SET_BYTE_NUMBER(fcinfo, total_actual_field_size);

		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("Expected line delimiter but found more data")));
	}

	/* add length of line. If we are not on the last line, add line delimiter as well.
	 * Don't add a new line length at the last line */
	data_cur += linelen;
	if (!is_last_line)
		data_cur += format_config.line_delimiter_length;
	pfree(cur_line);
	/*
	 * set context for next iteration
	 * pack and return tuple.
	 */
	MemoryContextSwitchTo(oldcontext);
	FORMATTER_SET_DATACURSOR(fcinfo, data_cur);
	tuple = heap_form_tuple(tupdesc, my_data->values, my_data->nulls);
	/* hack... pass tuple here. don't free prev tuple - the executor does it  */
	((FormatterData*) fcinfo->context)->fmt_tuple = tuple;
	FORMATTER_RETURN_TUPLE(tuple);
}
