/*-------------------------------------------------------------------------
 *
 * version.c
 *	 Returns the PostgreSQL version string
 *
 * IDENTIFICATION
 *
 * $PostgreSQL: pgsql/src/backend/utils/adt/version.c,v 1.13 2003/11/29 19:52:00 pgsql Exp $
 * $Name:  $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/builtins.h"


Datum
pgsql_version(PG_FUNCTION_ARGS)
{
	char tag[] = " $Name: $";
	char version[512];
	int			n;
	text	   *ret;
	strcpy(version, PG_VERSION_STR " compiled on " __DATE__ " " __TIME__);
	
	if (strlen(tag) > 10)
	{
		strcat(version, tag);
	}
	n = strlen(version);
		
	ret = (text *) palloc(n + VARHDRSZ);

	VARATT_SIZEP(ret) = n + VARHDRSZ;
	memcpy(VARDATA(ret), version, n);

	PG_RETURN_TEXT_P(ret);
}
