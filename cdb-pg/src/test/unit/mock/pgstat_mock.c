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
#include "utils/relcache.h"

bool		pgstat_track_counts = false;

void
pgstat_initstats(Relation rel)
{
	check_expected(rel);
	mock();
}

void
pgstat_count_heap_update(Relation rel, bool hot)
{
	check_expected(rel);
	check_expected(hot);
	mock();
}

void
pgstat_count_heap_insert(Relation rel)
{
	check_expected(rel);
	mock();
}

void pgstat_reset_all(void)
{
	mock();
}

void
pgstat_count_heap_delete(Relation rel)
{
	check_expected(rel);
	mock();
}
