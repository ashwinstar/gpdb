/*-------------------------------------------------------------------------
 *
 * cdbsubplan.h
 * routines for preprocessing initPlan subplans
 *		and executing queries with initPlans *
 * Copyright (c) 2003-2005, Greenplum
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBSUBPLAN_H
#define CDBSUBPLAN_H

#include "executor/execdesc.h"
#include "nodes/params.h"

extern void preprocess_initplans(QueryDesc *queryDesc);
extern ParamListInfo addRemoteExecParamsToParamList(Plan *plan, ParamListInfo p, ParamExecData *prm, int nprm);

#endif   /* CDBSUBPLAN_H */
