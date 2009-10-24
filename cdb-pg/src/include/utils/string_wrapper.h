/*-----------------------------------------------------------------------
 *
 * Wrappers around string.h functions
 *
 * $PostgreSQL: pgsql/src/include/utils/string_wrapper.h,v 1.27 2009/01/01 17:24:02 cwhipkey Exp $
 *
 * Copyright (c) 2009, Greenplum Inc.
 *
 *-----------------------------------------------------------------------
 */

#ifndef _UTILS___STRING_WRAPPER_H
#define _UTILS___STRING_WRAPPER_H

#include <string.h>
#include <errno.h>
#include <utils/elog.h>

static inline
size_t gp_strxfrm(char *dst, const char *src, size_t n)
{
	size_t result;

	errno = 0;
	result = strxfrm(dst, src, n);

	if ( errno != 0 )
	{
		if ( errno == EINVAL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER), \
							errmsg("Failed to transform string because it contains characters outside of the domain of the collating sequence.  String: '%s'",
															src)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR), \
							errmsg("Unable to transform string.  Error code %d", errno)));
		}
	}

	return result;
}

#endif   /* _UTILS___STRING_WRAPPER_H */
