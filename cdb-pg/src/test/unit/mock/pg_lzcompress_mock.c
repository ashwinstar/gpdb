/*
 * Manually created mocking source
 *
 * This file is not automatically updated by generate_mocks.py
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <limits.h>
#include "utils/pg_lzcompress.h"

const PGLZ_Strategy *const PGLZ_strategy_default;
const PGLZ_Strategy *const PGLZ_strategy_always;
bool 
pglz_compress(const char *source, int32 slen, PGLZ_Header * dest, const PGLZ_Strategy * strategy)
{
	check_expected(source);
	check_expected(slen);
	check_expected(dest);
	check_expected(strategy);
	optional_assignment(dest);
	return (bool) mock();
}

void 
pglz_decompress(const PGLZ_Header * source, char *dest)
{
	check_expected(source);
	check_expected(dest);
	optional_assignment(dest);
	mock();
}
