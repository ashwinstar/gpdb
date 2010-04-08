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
#define SAFE_STR_LENGTH(s) ((s) == NULL ? 0 : strlen(s))

static inline
int gp_strcoll(const char *left, const char *right)
{
	int result;

	errno = 0;
	result = strcoll(left, right);

	if ( errno != 0 )
	{
		if ( errno == EINVAL || EILSEQ)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
							errmsg("Unable to compare strings because one or both contained data that is not valid "
							       "for the collation specified by LC_COLLATE ('%s').  First string has length %lu "
							       "and value (limited to 100 characters): '%.100s'.  Second string has length %lu "
							       "and value (limited to 100 characters): '%.100s'",
									GetConfigOption("lc_collate"),
									(unsigned long) SAFE_STR_LENGTH(left),
									NULL_TO_DUMMY_STR(left),
									(unsigned long) SAFE_STR_LENGTH(left),
									NULL_TO_DUMMY_STR(right))));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
							errmsg("Unable to compare strings.  "
							       "Error: %s.  "
							       "First string has length %lu and value (limited to 100 characters): '%.100s'.  "
							       "Second string has length %lu and value (limited to 100 characters): '%.100s'",
									strerror(errno),
									(unsigned long) SAFE_STR_LENGTH(left),
									NULL_TO_DUMMY_STR(left),
									(unsigned long) SAFE_STR_LENGTH(left),
									NULL_TO_DUMMY_STR(right))));
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
		if ( errno == EINVAL || errno == EILSEQ)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
							errmsg("Unable to process string for comparison or sorting because it contains data that "
							        "is not valid for the collation specified by LC_COLLATE ('%s').  "
							        "The string has length %lu "
							        "and value (limited to 100 characters): '%.100s'",
								    GetConfigOption("lc_collate"),
									(unsigned long) SAFE_STR_LENGTH(src),
									NULL_TO_DUMMY_STR(src))));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
							errmsg("Unable to process string for comparison or sorting.  Error: %s.  "
							        "String has length %lu and value (limited to 100 characters): '%.100s'",
									strerror(errno),
									(unsigned long) SAFE_STR_LENGTH(src),
									NULL_TO_DUMMY_STR(src))));
		}
	}

	return result;
}

#endif   /* _UTILS___STRING_WRAPPER_H */
