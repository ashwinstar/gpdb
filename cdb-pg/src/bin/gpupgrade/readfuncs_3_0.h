/*
 * 3.0 read funcs
 *
 * $PostgreSQL$
 */

#ifndef READFUNCS_3_0_H
#define READFUNCS_3_0_H

#include "nodes/nodes.h"

extern void *string30toNode(char *str);
extern Node *parse30NodeString(void);

#endif /* READFUNCS_3_0_H */
