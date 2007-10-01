#ifndef GPCC_STRING_H
#define GPCC_STRING_H

#include <gpcc.h>
#include <apr_strings.h>

APR_DECLARE(char*) gpcc_string_trim(char* s);
APR_DECLARE(char*) gpcc_string_tolower(char* s);
APR_DECLARE(apr_int64_t) gpcc_string_parse_memory(const char* s);


#endif /*GPCC_STRING_H*/
