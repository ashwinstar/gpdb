/*---------------------------------------------------------------------
 * Copyright (c) 2012 EMC Corporation All Rights Reserved
 *
 * This is a random number generator (RNG) implementation and its
 * interface to the SQL random() function.  This file consists of
 * two parts; one is the RNG implementation copied from
 * cdb-pg/src/port/rand.c, which was originally copied for win32
 * portability.  The other part is the actual SQL random function
 * and a tweak for our distributed system.
 *
 * The system's traditional RNG function was assumed to be called in
 * a single process program.  We, however, need to make this RNG be
 * aware of our distributed processes, and make use of a technique
 * called "leap-frogging."  When a query starts, each process sets
 * the same seed and obtains the same random number sequence but moves
 * to different starting points.  Once random function is called,
 * it leaps to the next position by the number of co-working processes.
 * This way we can emulate a single sequence RNG from the distributed
 * processes without inter-process communication.
 *
 * Each process needs to know  1) the seed value, 2) its starting
 * position, and 3) the number of processes running simultaneously.
 * 1) is stored in QD and obtained when the query is dispatched.
 * 2) and 3) are obtained in the query executor.  get_seed_per_query()
 * and set_seed_per_query() are the interfaces to the executor.
 * After the query executor starts, drandom() will return a pseudo
 * random number and leaps to the next position.  Since the seed is
 * now stored in QD process, we disallow the use of setseed() function.
 * (Imagine if setseed() is called in the segment;  there's no way
 * to set the seed value in QD in that case.)  Users still make use
 * of SET seed command to set an arbitrary seed value.
 *
 * We could make use of the system's lrand48() function for our leap-
 * frogging, but as we need an exclusive control over the state behind
 * the RNG function (think about a third-party code calls the system's
 * lrand48 in between the query execution along with our distributed
 * RNG), we have our own private implementation in this file.  We might
 * be able to have better well-known RNG algorithm than this, but the
 * rule in GPDB is that when we introduce a new RNG algorithm, it
 * should be the one certified and compliant with FIPS-140.  For now we
 * utilize the algorithm that has been in our source code rather than
 * introducing a new one.
 *--------------------------------------------------------------------*/
/*---------------------------------------------------------------------
 * code from src/port/rand.c
 *--------------------------------------------------------------------*/
#include "c.h"

/*
 * Copyright (c) 1993 Martin Birgmeier
 * All rights reserved.
 *
 * You may redistribute unmodified or modified versions of this source
 * code provided that the above copyright notice and this and the
 * following conditions are retained.
 *
 * This software is provided ``as is'', and comes with no warranties
 * of any kind. I shall in no event be liable for anything that happens
 * to anyone/anything when using this software.
 */
#define RAND48_SEED_0	(0x330e)
#define RAND48_SEED_1	(0xabcd)
#define RAND48_SEED_2	(0x1234)
#define RAND48_MULT_0	(0xe66d)
#define RAND48_MULT_1	(0xdeec)
#define RAND48_MULT_2	(0x0005)
#define RAND48_ADD		(0x000b)

static unsigned short _rand48_seed[3] = {
	RAND48_SEED_0,
	RAND48_SEED_1,
	RAND48_SEED_2
};
static unsigned short _rand48_mult[3] = {
	RAND48_MULT_0,
	RAND48_MULT_1,
	RAND48_MULT_2
};
static unsigned short _rand48_add = RAND48_ADD;

static void
_dorand48(unsigned short xseed[3])
{
	unsigned long accu;
	unsigned short temp[2];

	accu = (unsigned long) _rand48_mult[0] * (unsigned long) xseed[0] +
		(unsigned long) _rand48_add;
	temp[0] = (unsigned short) accu;	/* lower 16 bits */
	accu >>= sizeof(unsigned short) * 8;
	accu += (unsigned long) _rand48_mult[0] * (unsigned long) xseed[1] +
		(unsigned long) _rand48_mult[1] * (unsigned long) xseed[0];
	temp[1] = (unsigned short) accu;	/* middle 16 bits */
	accu >>= sizeof(unsigned short) * 8;
	accu += _rand48_mult[0] * xseed[2] + _rand48_mult[1] * xseed[1] + _rand48_mult[2] * xseed[0];
	xseed[0] = temp[0];
	xseed[1] = temp[1];
	xseed[2] = (unsigned short) accu;
}

static long
gp_lrand48(void)
{
	_dorand48(_rand48_seed);
	return ((long) _rand48_seed[2] << 15) + ((long) _rand48_seed[1] >> 1);
}

static void
gp_srand48(long seed)
{
	_rand48_seed[0] = RAND48_SEED_0;
	_rand48_seed[1] = (unsigned short) seed;
	_rand48_seed[2] = (unsigned short) (seed > 16);
	_rand48_mult[0] = RAND48_MULT_0;
	_rand48_mult[1] = RAND48_MULT_1;
	_rand48_mult[2] = RAND48_MULT_2;
	_rand48_add = RAND48_ADD;
}

/*---------------------------------------------------------------------
 * GPDB
 *--------------------------------------------------------------------*/
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/gp_rand.h"

/*
 * Our random() function is based on leap-frogging approach to
 * deal with distributed RNG problem.  This value tells how many
 * positions to skip in the pseudo random number sequence.  Note,
 * in the utility mode, it's just 0 and works fine as in the single node.
 */
static int RandomLeapWidth = 0;

/*
 * The seed value and query id.  gp_random_seed is set in the SET seed
 * command. gp_random_query_id is reset in the SET command and is
 * incremented every time a new query runs.
 */
static int gp_random_seed = -1;
static int gp_random_query_id = -1;

/*
 * Equivalent to PG's random/srandom().  You could add another RNG
 * implementation here and switch with some flags in them.
 */
static long
gp_random(void)
{
	return gp_lrand48();
}

static void
gp_srandom(long seed)
{
	gp_srand48(seed);
}

/*
 * drandom		- returns a random number in a distributed way
 */
Datum
drandom(PG_FUNCTION_ARGS)
{
	float8		result;
	int			i;

	result = (double) gp_random();

	/* result [0.0 - 1.0) */
	result = result / ((double) MAX_RANDOM_VALUE + 1);

	/* leap to the next number. */
	for (i = 1; i < RandomLeapWidth; i++)
		gp_random();

	PG_RETURN_FLOAT8(result);
}


/*
 * setseed		- deprecated.
 */
Datum
setseed(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("setseed() is deprecated"),
			 errhint("Use SET seed.")));

	PG_RETURN_VOID();   /* keep compiler quiet */
}

/*
 * get_seed_per_query -
 */
uint32
get_seed_per_query(void)
{
	gp_random_query_id++;

	/* seed == -1 indicates the variable has not been initialized. */
	if (gp_random_seed == -1)
		gp_random_seed = (int) (time(NULL) % 0x7fffff);

	return ((uint32) gp_random_seed << 16 | (uint32) gp_random_query_id);
}

/*
 * set_seed_per_query -
 *
 * set local seed for this query and set up leap-frogging.
 */
void
set_seed_per_query(unsigned int seed, int init_pos, int leap_width)
{
	int		i;

	gp_srandom((long) seed);
	for (i = 0; i < init_pos; i++)
		gp_random();

	RandomLeapWidth = leap_width;
}

/*
 * Random number seed
 */
bool
assign_random_seed(double value, bool doit, GucSource source)
{
	/* Can't really roll back on error, so ignore non-interactive setting */
	if (doit && source >= PGC_S_INTERACTIVE)
	{
		/* value is [0, 1] */
		gp_random_seed = value * 0xffff;
		gp_random_query_id = -1;
	}
	return true;
}

const char *
show_random_seed(void)
{
	return "unavailable";
}
