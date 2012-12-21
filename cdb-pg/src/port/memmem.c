/*-------------------------------------------------------------------------
 *
 * gpmemmem.c
 *	  Find the first occurrence of the byte string in another byte string.
 *
 */

#include "c.h"
#include <ctype.h>

/*
 * Return the first occurrence of needle in haystack, or NULL if needle doesn't appear.
 * using memcmp and not strcmp (i.e. works also for not null-terminate strings).
 *
 * Needed because OSX does not have memmem and linux does not have strnstr...
 */
void *
memmem (const void *haystack, size_t haystacklen,
		const void *needle, size_t needlelen)
{
	size_t i = 0;
	char const *curr = haystack;

	if (needlelen == 0)
		return (void *)haystack;

	if (needlelen > haystacklen || haystack == NULL || needle == NULL)
		return NULL;

	for (i = 0; i <= haystacklen - needlelen; i++)
	{
		if (memcmp(curr + i, needle, needlelen) == 0)
			return (void *)(curr + i);
	}

	return NULL;
}
