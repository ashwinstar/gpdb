/*
 * Auto-generated Mocking Source
 * 
 * Source: storage/s_lock.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <time.h>
#include <unistd.h>
#include "storage/s_lock.h"

void 
s_lock(volatile slock_t * lock, const char *file, int line)
{

    check_expected(lock);
    check_expected(file);
    check_expected(line);
    optional_assignment(lock);
    mock();

}

void 
set_spins_per_delay(int shared_spins_per_delay)
{

    check_expected(shared_spins_per_delay);

    mock();

}

int 
recompute_spins_per_delay(int shared_spins_per_delay)
{

    check_expected(shared_spins_per_delay);

    return (int) mock();

}
