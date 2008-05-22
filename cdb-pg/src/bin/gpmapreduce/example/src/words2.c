#include "gpmapreduce.h"

typedef struct
{
	int    field;
	int    position;
	int    len;
} map_weblog_state;

#define NUMFIELDS 10
const char fieldnames[NUMFIELDS][10] = {
	"hostname", "rfc931", "uname", "date", "timezone", 
	"req", "item", "protocol", "status", "bytes"
};

/*
 * map_weblog
 *   extracts fields from a weblog
 */
PG_FUNCTION_INFO_V1(map_weblog);
Datum map_weblog(PG_FUNCTION_ARGS)
{
	map_weblog_state *mystate;
	text             *inkey, *invalue;
	text             *outkey, *outvalue;
	int               i, len;

	/* Initialize */
	GPMAP_INIT(mystate, sizeof(mystate), inkey, TEXT, invalue, TEXT);
	if (GPMAP_IS_FIRSTCALL())
	{ 
		mystate->field = 0;
		mystate->position = VARHDRSZ;
		mystate->len = VARSIZE(invalue);
	}

	/* Make TEXT output key from CSTRING fieldname */
	len = strlen(fieldnames[mystate->field]);
	outkey = palloc(VARHDRSZ + len);
	SET_VARSIZE(outkey, VARHDRSZ + len);
	memcpy(VARDATA(outkey), fieldnames[mystate->field], len);

	/* scan ' ' delimited log file - needs better internationalization */
	for (i = mystate->position; i < mystate->len; i++) {
		if (((char*)invalue)[i] == ' ')
			break;
	}

	/* Make TEXT output value from current input value substring */
	outvalue = palloc(VARHDRSZ + i - mystate->position);
	SET_VARSIZE(outvalue, VARHDRSZ + i - mystate->position);
	memcpy(VARDATA(outvalue), ((char*)invalue) + mystate->position, 
		   i - mystate->position);

	/* Update state for next call */
	mystate->field++;
	mystate->position = i;

	/* If there is no more output return */
	if (mystate->field > NUMFIELDS)
		GPMAP_DONE();
	GPMAP_RETURN_NEXT(mystate, outkey, TEXT, outvalue, TEXT);
}
