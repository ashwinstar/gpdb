/*-------------------------------------------------------------------------
 *
 * cdbpathlocus.c
 *
 * Copyright (c) 2005-2006, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cdb/cdbcat.h"         /* MppPolicy */
#include "cdb/cdbdef.h"         /* CdbSwap() */
#include "cdb/cdbpullup.h"      /* cdbpullup_missing_var_walker() */
#include "nodes/makefuncs.h"    /* makeVar() */
#include "nodes/plannodes.h"    /* Plan */
#include "nodes/relation.h"     /* RelOptInfo */
#include "optimizer/pathnode.h" /* Path */
#include "optimizer/paths.h"    /* cdb_build_distribution_pathkeys() */
#include "optimizer/tlist.h"    /* tlist_member() */
#include "parser/parse_expr.h"  /* exprType() and exprTypmod() */

#include "cdb/cdbvars.h"
#include "cdb/cdbpathlocus.h"   /* me */


/*
 * This flag controls the policy type returned from
 * cdbpathlocus_from_baserel() for non-partitioned tables.
 * It's a kludge put in to allow us to do
 * distributed queries on catalog tables, like pg_class
 * and pg_statistic.
 * To use it, simply set it to true before running a catalog query, then set
 * it back to false.
 */
bool cdbpathlocus_querysegmentcatalogs = false;


/*
 * cdbpathlocus_compare
 *
 *    - Returns false if locus a or locus b is "strewn", meaning that it
 *      cannot be determined whether it is equal to another partitioned
 *      distribution.
 *
 *    - Returns true if a and b have the same 'locustype' and 'partkey'.
 *
 *    - Returns true if both a and b are hashed and the set of possible
 *      m-tuples of expressions (e1, e2, ..., em) produced by a's partkey
 *      is equal to (if op == CdbPathLocus_Equal) or a superset of (if
 *      op == CdbPathLocus_Contains) the set produced by b's partkey.
 *
 *    - Returns false otherwise.
 */
bool
cdbpathlocus_compare(CdbPathLocus_Comparison    op,
                     CdbPathLocus               a,
                     CdbPathLocus               b)
{
    ListCell   *acell;
    ListCell   *bcell;
    ListCell   *aequivpathkeycell;
    ListCell   *bequivpathkeycell;

    Assert(op == CdbPathLocus_Comparison_Equal ||
           op == CdbPathLocus_Comparison_Contains);

    if (CdbPathLocus_IsStrewn(a) ||
        CdbPathLocus_IsStrewn(b))
        return false;

    if (CdbPathLocus_IsEqual(a, b))
        return true;

    if (!a.partkey ||
        !b.partkey ||
        CdbPathLocus_Degree(a) != CdbPathLocus_Degree(b))
        return false;

    if (a.locustype == b.locustype)
    {
        if (CdbPathLocus_IsHashed(a))
        {
            forboth(acell, a.partkey, bcell, b.partkey)
            {
                List   *apathkey = (List *)lfirst(acell);
                List   *bpathkey = (List *)lfirst(bcell);
                if (apathkey != bpathkey)
                    return false;
            }
            return true;
        }

        if (CdbPathLocus_IsHashedOJ(a))
        {
            forboth(acell, a.partkey, bcell, b.partkey)
            {
                List   *aequivpathkeylist = (List *)lfirst(acell);
                List   *bequivpathkeylist = (List *)lfirst(bcell);
                foreach(bequivpathkeycell, bequivpathkeylist)
                {
                    List   *bpathkey = (List *)lfirst(bequivpathkeycell);
                    if (!list_member_ptr(aequivpathkeylist, bpathkey))
                        return false;
                }
                if (op == CdbPathLocus_Comparison_Equal)
                {
                    foreach(aequivpathkeycell, aequivpathkeylist)
                    {
                        List   *apathkey = (List *)lfirst(aequivpathkeycell);
                        if (!list_member_ptr(bequivpathkeylist, apathkey))
                            return false;
                    }
                }
            }
            return true;
        }
    }

    if (CdbPathLocus_IsHashedOJ(a) &&
        CdbPathLocus_IsHashed(b))
    {
        if (op == CdbPathLocus_Comparison_Equal)
            CdbSwap(CdbPathLocus, a, b);
        else
        {
            forboth(acell, a.partkey, bcell, b.partkey)
            {
                List   *aequivpathkeylist = (List *)lfirst(acell);
                List   *bpathkey = (List *)lfirst(bcell);
                if (!list_member_ptr(aequivpathkeylist, bpathkey))
                    return false;
            }
            return true;
        }
    }

    if (CdbPathLocus_IsHashed(a) &&
        CdbPathLocus_IsHashedOJ(b))
    {
        forboth(acell, a.partkey, bcell, b.partkey)
        {
            List   *apathkey = (List *)lfirst(acell);
            List   *bequivpathkeylist = (List *)lfirst(bcell);
            foreach(bequivpathkeycell, bequivpathkeylist)
            {
                List   *bpathkey = (List *)lfirst(bequivpathkeycell);
                if (apathkey != bpathkey)
                    return false;
            }
        }
        return true;
    }

    Assert(false);
    return false;
}                               /* cdbpathlocus_compare */


/*
 * cdbpathlocus_from_baserel
 *
 * Returns a locus describing the distribution of a base relation.
 */
CdbPathLocus
cdbpathlocus_from_baserel(struct PlannerInfo   *root,
                          struct RelOptInfo    *rel)
{
    CdbPathLocus result;
    MppPolicy   *policy = rel->cdbpolicy;
	
	if ( Mpp_role != MPP_ROLE_DISPATCH )
	{
		CdbPathLocus_MakeRoot(&result);
		return result;
	}


    if (policy &&
        policy->ptype == POLICYTYPE_PARTITIONED)
    {
        List *partkey = cdb_build_distribution_pathkeys(root,
                                                        rel,
                                                        policy->nattrs,
                                                        policy->attrs);
        CdbPathLocus_MakeHashed(&result, partkey);
    }

    /* Kludge used internally for querying catalogs on segment dbs */
    else if (cdbpathlocus_querysegmentcatalogs)
        CdbPathLocus_MakeStrewn(&result);

    /* Normal catalog access */
    else
        CdbPathLocus_MakeRoot(&result);

    return result;
}                               /* cdbpathlocus_from_baserel */


/*
 * cdbpathlocus_from_subquery
 *
 * Returns a locus describing the distribution of a subquery.
 * The subquery plan should have been generated already.
 */
CdbPathLocus
cdbpathlocus_from_subquery(struct PlannerInfo   *root,
                           struct RelOptInfo    *rel)
{
    CdbPathLocus    locus;
    Plan           *plan = rel->subplan;
    Flow           *flow = plan->flow;

	if ( Mpp_role != MPP_ROLE_DISPATCH )
	{
		CdbPathLocus_MakeRoot(&locus);
		return locus;
	}
	
    CdbPathLocus_MakeNull(&locus);

    /* Absence of a Flow node means the plan contains operators
     * whose parallelization is delegated to the post-parallelizer.
     * We don't know the output locus because the p.p. isn't
     * called until later.
     */
    if (!flow)
        goto done;

    /* Flow node was made from CdbPathLocus by cdbpathtoplan_create_flow() */
    switch (flow->flotype)
    {
        case FLOW_SINGLETON:
            if (flow->segindex == -1)
                CdbPathLocus_MakeRoot(&locus);
            else
                CdbPathLocus_MakeSingleQE(&locus);
            break;
        case FLOW_REPLICATED:
            CdbPathLocus_MakeReplicated(&locus);
            break;
        case FLOW_PARTITIONED:
        {
            List       *partkey = NIL;
            ListCell   *hashexprcell;
            List       *eq = list_make1(makeString("="));
            foreach(hashexprcell, flow->hashExpr)
            {
                Node           *expr = (Node *)lfirst(hashexprcell);
                TargetEntry    *tle = tlist_member(expr, plan->targetlist);
                Var            *var;
                List           *pathkey;
                if (!tle)
                    break;

				Assert(tle->resno >= 1);
                var = makeVar(rel->relid,
                              tle->resno,
                              exprType((Node *) tle->expr),
                              exprTypmod((Node *) tle->expr),
                              0);
                pathkey = cdb_make_pathkey_for_expr(root, (Node *)var, eq);
                partkey = lappend(partkey, pathkey);
            }
            if (partkey &&
                !hashexprcell)
                CdbPathLocus_MakeHashed(&locus, partkey);
            else
                CdbPathLocus_MakeStrewn(&locus);
            list_free_deep(eq);
            break;
        }
        default:
            Assert(false);
    }

done:
    if (CdbPathLocus_IsNull(locus))
        ereport(ERROR, (errcode(ERRCODE_MPP_FEATURE_NOT_YET),
                        errmsg("Unsupported subquery")
                       ));
    return locus;
}                               /* cdbpathlocus_from_subquery */


/*
 * cdbpathlocus_get_pathkeyitem
 *
 * Searches the equivalence class 'pathkey' for a PathKeyItem that
 * uses no rels outside the 'relids' set, and either is a member of
 * 'targetlist', or uses no Vars that are not in 'targetlist'.
 * Returns  NULL if not found.
 */
static PathKeyItem *
cdbpathlocus_get_pathkeyitem(List          *pathkey,
                             Relids         relids,
                             List          *targetlist)
{
    ListCell   *pathkeycell;

    foreach(pathkeycell, pathkey)
    {
        PathKeyItem    *item = lfirst(pathkeycell);
        Assert(IsA(item, PathKeyItem));

        /* Constant expr?  Return it. */
        if (item->cdb_num_relids == 0)
            return item;

        /* Consider this item if all of its rels are in 'relids'. */
        if (bms_is_subset(item->cdb_key_relids, relids))
        {
            /* Return this item if the whole expr is in targetlist. */
            if (tlist_member(item->key, targetlist))
                return item;

            /* Return this item if all referenced Vars are in targetlist. */
            if (!IsA(item->key, Var) &&
                !cdbpullup_missing_var_walker(item->key, targetlist))
                return item;
        }
    }
    return NULL;
}                               /* cdbpathlocus_get_pathkeyitem */


/*
 * cdbpathlocus_get_partkey_exprs
 *
 * Returns a List with one Expr for each partkey column.  Each item either is
 * in the given targetlist, or has no Var nodes that are not in the targetlist;
 * and uses only rels in the given set of relids.  Returns NIL if the
 * partkey cannot be expressed in terms of the given relids and targetlist.
 */
List *
cdbpathlocus_get_partkey_exprs(CdbPathLocus     locus,
                               Bitmapset       *relids,
                               List            *targetlist)
{
    List       *result = NIL;
    ListCell   *partkeycell;
    foreach(partkeycell, locus.partkey)
    {
        List           *pathkey;
        PathKeyItem    *item = NULL;

        if (CdbPathLocus_IsHashed(locus))
        {
            pathkey = lfirst(partkeycell);
            item = cdbpathlocus_get_pathkeyitem(pathkey, relids, targetlist);
        }
        else if (CdbPathLocus_IsHashedOJ(locus))
        {
            List       *pathkeylist = lfirst(partkeycell);
            ListCell   *pathkeylistcell;
            foreach(pathkeylistcell, pathkeylist)
            {
                pathkey = lfirst(pathkeylistcell);
                item = cdbpathlocus_get_pathkeyitem(pathkey,
                                                    relids,
                                                    targetlist);
                if (item)
                    break;
            }
        }
        else
            Assert(0);

        /* Fail if can't evaluate partkey in the context of this targetlist. */
        if (!item)
            return NIL;

        result = lappend(result, copyObject(item->key));
    }
    return result;
}                               /* cdbpathlocus_get_partkey_exprs */


/*
 * cdbpathlocus_join
 */
CdbPathLocus
cdbpathlocus_join(CdbPathLocus a, CdbPathLocus b)
{
    ListCell       *acell;
    ListCell       *bcell;
    List           *equivpathkeylist;
    CdbPathLocus    ojlocus;

    /* Do both input rels have same locus? */
    if (cdbpathlocus_compare(CdbPathLocus_Comparison_Equal, a, b))
        return a;

    /* Join will be done on qDisp if either rel is on qDisp,
     * because qDisp can only receive rows, not send them (for now).
     */
    if (CdbPathLocus_IsRoot(a))
        return a;
    if (CdbPathLocus_IsRoot(b))
        return b;

    /* If one rel is general or replicated, result stays with the other rel. */
    if (CdbPathLocus_IsGeneral(a) ||
        CdbPathLocus_IsReplicated(a))
        return b;
    if (CdbPathLocus_IsGeneral(b) ||
        CdbPathLocus_IsReplicated(b))
        return a;

    /* This is an outer join, or one or both inputs are outer join results. */

    Assert(CdbPathLocus_Degree(a) == CdbPathLocus_Degree(b));

    if (CdbPathLocus_IsHashed(a) &&
        CdbPathLocus_IsHashed(b))
    {
        /* Zip the two pathkey lists together to make a HashedOJ locus. */
        CdbPathLocus_MakeHashedOJ(&ojlocus, NIL);
        forboth(acell, a.partkey, bcell, b.partkey)
        {
            List   *apathkey = (List *)lfirst(acell);
            List   *bpathkey = (List *)lfirst(bcell);
            equivpathkeylist = list_make2(apathkey, bpathkey);
            ojlocus.partkey = lappend(ojlocus.partkey, equivpathkeylist);
        }
        return ojlocus;
    }

    if (!CdbPathLocus_IsHashedOJ(a))
        CdbSwap(CdbPathLocus, a, b);

    Assert(CdbPathLocus_IsHashedOJ(a));
    Assert(CdbPathLocus_IsHashed(b) ||
           CdbPathLocus_IsHashedOJ(b));

    CdbPathLocus_MakeHashedOJ(&ojlocus, NIL);
    forboth(acell, a.partkey, bcell, b.partkey)
    {
        List   *aequivpathkeylist = (List *)lfirst(acell);
        if (CdbPathLocus_IsHashed(b))
        {
            List   *bpathkey = (List *)lfirst(bcell);
            equivpathkeylist = lappend(list_copy(aequivpathkeylist), bpathkey);
        }
        else
        {
            List   *bequivpathkeylist = (List *)lfirst(bcell);
            equivpathkeylist = list_union_ptr(aequivpathkeylist,
                                              bequivpathkeylist);
        }
        ojlocus.partkey = lappend(ojlocus.partkey, equivpathkeylist);
    }
    return ojlocus;
}                               /* cdbpathlocus_join */

