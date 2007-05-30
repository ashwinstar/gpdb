#include "gpcc_strings.h"
#include <stdlib.h>

APR_DECLARE(char*) gpcc_string_trim(char* s)
{
    char* p;
    s += strspn(s, " \t\r\n");
    for (p = s + strlen(s) - 1; p > s; p--) {
	if (strchr(" \t\r\n", *p))
	    *p = 0;
	else
	    break;
    }
    return s;
}

#if defined(WIN32)&&defined(_MSC_VER)
#define strtoll _strtoi64
#endif

APR_DECLARE(apr_int64_t) gpcc_string_parse_memory(const char* s)
{
    const char* p = s + strspn(s, " \t");
    const char* q = p + strspn(p, "0123456789");
    int factor = 1;
    apr_int64_t num = strtoll(p, 0, 0);

    switch (*q++) {
    case 'k': case 'K':
	factor = 1024;
	break;
    case 'm': case 'M': 
	factor = 1024*1024;
	break;
    case 'g': case 'G':
	factor = 1024*1024*1024;
	break;
    case 'b': case 'B': 
	factor = 1;
	break;
    case 0:
	goto done;
    default:
	return 0;
    }

    switch (*q++) {
    case 'b': case 'B': 
	if (factor == 1) 
	    return 0;
	break;
    case 0:
	goto done;
    default:
	return 0;
    }

    done:
    return num * factor;
}
