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

#define NULL_TO_DUMMY_STR(s) ((s) == NULL ? "<<null>>" : (s))

static inline
int gp_strcoll(const char *left, const char *right)
{
	int result;

	errno = 0;
	result = strcoll(left, right);

	if ( errno != 0 )
	{
		if ( errno == EINVAL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
							errmsg("Failed to compare strings because of characters outside of the domain of the collating sequence.  First string: '%s' Second string: '%s'",
									NULL_TO_DUMMY_STR(left), NULL_TO_DUMMY_STR(right))));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
							errmsg("Unable to compare strings.  Error code %d.  First string: '%s' Second string: '%s'", errno, NULL_TO_DUMMY_STR(left), NULL_TO_DUMMY_STR(right))));
		}
	}

	return result;
}

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
					(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
							errmsg("Failed to transform string because it contains characters outside of the domain of the collating sequence.  String: '%s'",
									NULL_TO_DUMMY_STR(src))));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
							errmsg("Unable to transform string.  Error code %d", errno)));
		}
	}

	return result;
}

#endif   /* _UTILS___STRING_WRAPPER_H */
