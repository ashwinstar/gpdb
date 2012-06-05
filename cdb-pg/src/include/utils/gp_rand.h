/*-------------------------------------------------------------------------
 *
 * gp_rand.h
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_RAND_H
#define GP_RAND_H

#include "fmgr.h"
#include "utils/guc.h"

extern Datum drandom(PG_FUNCTION_ARGS);
extern Datum setseed(PG_FUNCTION_ARGS);
extern uint32 get_seed_per_query(void);
extern void set_seed_per_query(unsigned int seed, int init_pos, int leap_width);
extern bool assign_random_seed(double value, bool doit, GucSource source);
extern const char *show_random_seed(void);

#endif   /* GP_RAND_H */
