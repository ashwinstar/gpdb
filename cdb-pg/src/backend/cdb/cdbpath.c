/*-------------------------------------------------------------------------
 *
 * cdbpath.c
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"          /* work_mem */
#include "nodes/relation.h"     /* RelOptInfo */
#include "optimizer/cost.h"     /* cpu_tuple_cost */
#include "optimizer/pathnode.h" /* Path */
#include "optimizer/paths.h"    /* compare_pathkeys() */

#include "cdb/cdbllize.h"       /* makeFlow() */
#include "cdb/cdbvars.h"        /* CdbIdentity */

#include "cdb/cdbpath.h"        /* me */


/* Globals */
Cost        cdb_motion_tuple_cost   = 0.0;


/*
 * cdbpath_add_flow_to_plan
 */
void
cdbpath_add_flow_to_plan(Query *root,
                         Path  *path,
                         Plan  *plan)
{
    Flow       *flow;
    
    if (CdbPathLocus_IsRoot(path->locus))
    {
        flow = makeFlow(FLOW_SINGLETON);
        flow->segindex = -1;
    }
    else if (CdbPathLocus_IsReplicated(path->locus) ||
             CdbPathLocus_IsGeneral(path->locus))
    {
        flow = makeFlow(FLOW_REPLICATED);
    }
    else if (CdbPathLocus_IsHashed(path->locus) ||
             CdbPathLocus_IsHashedOJ(path->locus))
    {
        flow = makeFlow(FLOW_PARTITIONED);
        flow->hashExpr = cdbpathlocus_get_partkey_exprs(path->locus,
                                                        path->parent->relids);
    }
    else
        Assert(false);
    
    plan->flow = flow;
}                               /* cdbpath_add_flow_to_plan */


/*
 * cdbpath_create_motion_path
 *    Returns a Path that delivers the subpath result to the 
 *    given locus, or NULL if it can't be done.  
 */
Path *
cdbpath_create_motion_path(Path            *subpath,    
                           List            *pathkeys,  
                           CdbPathLocus     locus)
{
    CdbMotionPath      *pathnode;
    bool                hashToSelfOnly = false;

    /* No motion needed if subpath locus matches caller's request. */
    if (CdbPathLocus_IsEqual(subpath->locus, locus) ||
        cdbpathlocus_compare(CdbPathLocus_Comparison_Equal, subpath->locus, locus))
        return subpath;

    Assert(CdbPathLocus_IsRoot(locus) ||
           CdbPathLocus_IsReplicated(locus) ||
           CdbPathLocus_IsHashed(locus) ||
           CdbPathLocus_IsHashedOJ(locus));
    Assert(CdbPathLocus_IsValidType(subpath->locus));

    if (CdbPathLocus_IsRoot(locus))
    {
        /* replicated>>root would send redundant copies to the qDisp. */
        if (CdbPathLocus_IsReplicated(subpath->locus))
            return NULL;
    }
    else
    {
        /* qDisp can't send tuples to qExecs. */
        if (CdbPathLocus_IsRoot(subpath->locus))
            return NULL;

        /*
         * CDB TODO: handle general>>partitioned and replicated>>partitioned
         * by filtering on hash key (without actual motion)
         */
        if (CdbPathLocus_IsHashed(locus) ||
            CdbPathLocus_IsHashedOJ(locus))
        {
            if (CdbPathLocus_IsGeneral(subpath->locus) ||
                CdbPathLocus_IsReplicated(subpath->locus))
                return NULL;
        }

        /* qExecs can't merge sorted streams.  Result is unordered. */
        pathkeys = NIL;
    }

    /* Don't materialize before motion. */
    if (IsA(subpath, MaterialPath))
        subpath = ((MaterialPath *)subpath)->subpath;

    /* CDB TODO: If subpath->motionHazard, materialize before motion. */

    /* Allocate CdbMotionPath node. */
    pathnode = makeNode(CdbMotionPath);
    pathnode->path.pathtype = T_Motion;
    pathnode->path.parent = subpath->parent;
    pathnode->path.locus = locus;
    pathnode->path.pathkeys = pathkeys;
    pathnode->subpath = subpath;
    pathnode->hashToSelfOnly = hashToSelfOnly;

    pathnode->path.startup_cost = subpath->startup_cost;
    pathnode->path.total_cost = subpath->total_cost;
    pathnode->path.memory = subpath->memory;

    /*
     * If data is everywhere, no motion is needed; but the 
     * CdbMotionPath node can be used to relabel the subpath
     * with a specific locus.
     */
    if (CdbPathLocus_IsGeneral(subpath->locus))
    {
        pathnode->path.motionHazard = subpath->motionHazard;
        pathnode->path.isFinished = false;      /* CDB TODO: fix */
    }

    /*
     * Cost of motion
     */
    else
    {
        Cost    cost_per_row = (cdb_motion_tuple_cost > 0.0)
                             ? cdb_motion_tuple_cost
                             : cpu_tuple_cost;
        double  rows = IsA(subpath, UniquePath)
                     ? ((UniquePath *)subpath)->rows
                     : subpath->parent->rows;

        /* Does each receiver get whole rel? */
        if (CdbPathLocus_IsRoot(locus) ||
            CdbPathLocus_IsReplicated(locus))
            rows *= CdbIdentity.numsegments;

        pathnode->path.total_cost += rows * cost_per_row;

        pathnode->path.motionHazard = true;
        pathnode->path.isFinished = false;      /* CDB TODO: fix */
    }

    return (Path *)pathnode;
}                               /* cdbpath_create_motion_path */


/*
 * cdbpath_match_preds_to_partkey_tail
 *
 * Returns true if all of the locus's partitioning key expressions are
 * found as comparands of equijoin predicates in the mergeclause_list.
 *
 * NB: for mergeclause_list and pathkey structure assumptions, see:
 *          select_mergejoin_clauses() in joinpath.c
 *          find_mergeclauses_for_pathkeys() in pathkeys.c
 */

typedef struct 
{
    Query          *root;
    List           *mergeclause_list;
    CdbPathLocus    locus;
    CdbPathLocus   *colocus;
    bool            colocus_eq_locus;
} CdbpathMatchPredsContext;


static bool
cdbpath_match_preds_to_partkey_tail(CdbpathMatchPredsContext   *ctx,     
                                    ListCell                   *partkeycell)
{
    List       *copathkey = NIL;
    List       *sublist;
    ListCell   *rcell;

    sublist = (List *)lfirst(partkeycell);
    foreach(rcell, ctx->mergeclause_list)
    {
        RestrictInfo   *rinfo = (RestrictInfo *)lfirst(rcell);

        if (!rinfo->left_pathkey)
            cache_mergeclause_pathkeys(ctx->root, rinfo);

        if (CdbPathLocus_IsHashed(ctx->locus))
        {
            if (sublist == rinfo->left_pathkey)
                copathkey = rinfo->right_pathkey;
            else if (sublist == rinfo->right_pathkey)
                copathkey = rinfo->left_pathkey;
        }
        else if (CdbPathLocus_IsHashedOJ(ctx->locus))
        {
            if (list_member_ptr(sublist, rinfo->left_pathkey)) 
                copathkey = rinfo->right_pathkey;
            else if (rinfo->left_pathkey != rinfo->right_pathkey &&
                     list_member_ptr(sublist, rinfo->right_pathkey)) 
                copathkey = rinfo->left_pathkey;
        }

        if (copathkey)
            break; 
    }                

    /* Fail if didn't find an equijoin predicate on this partkey item. */
    if (!copathkey)
        return false;

    /* Might need to build co-locus if locus is outer join source or result. */
    if (copathkey != sublist)
        ctx->colocus_eq_locus = false;

    /* Match remaining partkey items. */
    partkeycell = lnext(partkeycell);
    if (partkeycell)
    {
        if (!cdbpath_match_preds_to_partkey_tail(ctx, partkeycell))
            return false;
    } 

    /* Success!  Matched all items.  Return co-locus if requested. */
    if (ctx->colocus)
    {
        if (ctx->colocus_eq_locus)
            *ctx->colocus = ctx->locus;
        else if (!partkeycell)
            CdbPathLocus_MakeHashed(ctx->colocus, list_make1(copathkey));
        else
            ctx->colocus->partkey = lcons(copathkey, ctx->colocus->partkey);
    }
    return true;
}                               /* cdbpath_match_preds_to_partkey_tail */


/*
 * cdbpath_match_preds_to_partkey
 *
 * Returns true if all of the locus's partitioning key expressions are
 * found as comparands of equijoin predicates in the mergeclause_list.
 *
 * Readers may refer also to these related functions:
 *          select_mergejoin_clauses() in joinpath.c
 *          find_mergeclauses_for_pathkeys() in pathkeys.c
 */
static bool
cdbpath_match_preds_to_partkey(Query           *root,
                               List            *mergeclause_list,
                               CdbPathLocus     locus,
                               CdbPathLocus    *colocus)            /* OUT */
{
    CdbpathMatchPredsContext ctx;

    Assert(CdbPathLocus_IsHashed(locus) ||
           CdbPathLocus_IsHashedOJ(locus));
    Assert(locus.partkey);

    ctx.root                = root;
    ctx.mergeclause_list    = mergeclause_list;
    ctx.locus               = locus;
    ctx.colocus             = colocus;
    ctx.colocus_eq_locus    = true;
    
    return cdbpath_match_preds_to_partkey_tail(&ctx, list_head(locus.partkey));
}                               /* cdbpath_match_preds_to_partkey */


/*
 * cdbpath_match_preds_to_both_partkeys
 *
 * Returns true if the mergeclause_list contains equijoin 
 * predicates between each item of the outer_locus partkey and
 * the corresponding item of the inner_locus partkey.
 *
 * Readers may refer also to these related functions:
 *          select_mergejoin_clauses() in joinpath.c
 *          find_mergeclauses_for_pathkeys() in pathkeys.c
 */
static bool
cdbpath_match_preds_to_both_partkeys(Query         *root,
                                     List          *mergeclause_list,
                                     CdbPathLocus   outer_locus,
                                     CdbPathLocus   inner_locus)
{
    ListCell   *outercell;
    ListCell   *innercell;

    if (!mergeclause_list ||
        !outer_locus.partkey || 
        !inner_locus.partkey ||
        CdbPathLocus_Degree(outer_locus) != CdbPathLocus_Degree(inner_locus))
        return false;

    forboth(outercell, outer_locus.partkey, innercell, inner_locus.partkey)
    {
        List        *outersublist = (List *)lfirst(outercell);
        List        *innersublist = (List *)lfirst(innercell);
        ListCell    *rcell;
        foreach(rcell, mergeclause_list)
        {
            RestrictInfo   *rinfo = (RestrictInfo *)lfirst(rcell);

            if (!rinfo->left_pathkey)
                cache_mergeclause_pathkeys(root, rinfo);

            /* Skip predicate if neither side matches outer partkey item. */ 
            if (CdbPathLocus_IsHashed(outer_locus))
            {
                if (outersublist != rinfo->left_pathkey &&
                    outersublist != rinfo->right_pathkey)
                    continue;
            }
            else 
            {
                Assert(CdbPathLocus_IsHashedOJ(outer_locus));
                if (!list_member_ptr(outersublist, rinfo->left_pathkey) &&
                    !list_member_ptr(outersublist, rinfo->right_pathkey)) 
                    continue;
            } 

            /* Skip predicate if neither side matches inner partkey item. */ 
            if (innersublist == outersublist)
            {}                  /* do nothing */
            else if (CdbPathLocus_IsHashed(inner_locus))
            {
                if (innersublist != rinfo->left_pathkey &&
                    innersublist != rinfo->right_pathkey)
                    continue;
            }
            else 
            {
                Assert(CdbPathLocus_IsHashedOJ(inner_locus));
                if (!list_member_ptr(innersublist, rinfo->left_pathkey) &&
                    !list_member_ptr(innersublist, rinfo->right_pathkey)) 
                    continue;
            } 

            /* Found equijoin between outer partkey item & inner partkey item */
            break;
        }

        /* Fail if didn't find equijoin between this pair of partkey items. */
        if (!rcell)
            return false;
    }
    return true;               
}                               /* cdbpath_match_preds_to_both_partkeys */


/*
 * cdbpath_partkeys_from_preds
 *
 * Makes a CdbPathLocus for repartitioning, driven by 
 * the equijoin predicates in the mergeclause_list.
 * Returns true if successful, or false if no usable equijoin predicates.
 *
 * Readers may refer also to these related functions:
 *      select_mergejoin_clauses() in joinpath.c
 *      make_pathkeys_for_mergeclauses() in pathkeys.c
 */
static bool
cdbpath_partkeys_from_preds(Query          *root,
                            List           *mergeclause_list, 
                            Path           *a_path,
                            CdbPathLocus   *a_locus,            /* OUT */
                            CdbPathLocus   *b_locus)            /* OUT */
{
    List       *a_partkey = NIL;
    List       *b_partkey = NIL;
    ListCell   *rcell;

    foreach(rcell, mergeclause_list)
    {
        RestrictInfo *rinfo = (RestrictInfo *)lfirst(rcell);

        if (!rinfo->left_pathkey)
        {
            cache_mergeclause_pathkeys(root, rinfo);
            Assert(rinfo->left_pathkey);
        }

        /* Left & right pathkeys are usually the same... */
        if (!b_partkey &&
            rinfo->left_pathkey == rinfo->right_pathkey)
        {
            if (!list_member_ptr(a_partkey, rinfo->left_pathkey))
                a_partkey = lappend(a_partkey, rinfo->left_pathkey);
        }

        /* ... except in outer join ON-clause. */
        else
        {
            List   *a_pathkey;
            List   *b_pathkey;

            if (bms_is_subset(rinfo->right_relids, a_path->parent->relids))
            {
                a_pathkey = rinfo->right_pathkey;
                b_pathkey = rinfo->left_pathkey;
            }
            else
            {
                a_pathkey = rinfo->left_pathkey;
                b_pathkey = rinfo->right_pathkey;
                Assert(bms_is_subset(rinfo->left_relids, a_path->parent->relids));
            }

            if (!b_partkey)
                b_partkey = list_copy(a_partkey);

            if (!list_member_ptr(a_partkey, a_pathkey) &&
                !list_member_ptr(b_partkey, b_pathkey))
            {
                a_partkey = lappend(a_partkey, a_pathkey);
                b_partkey = lappend(b_partkey, b_pathkey);
            }
        }

        if (list_length(a_partkey) >= 20)
            break;
    }

    if (!a_partkey)
        return false;

    CdbPathLocus_MakeHashed(a_locus, a_partkey);
    if (b_partkey)
        CdbPathLocus_MakeHashed(b_locus, b_partkey);
    else
        *b_locus = *a_locus;
    return true;
}                               /* cdbpath_partkeys_from_preds */


/*
 * cdbpath_motion_for_join
 *
 * Decides where a join should be done.  Adds Motion operators atop
 * the subpaths if needed to deliver their results to the join locus.  
 * Returns the join locus if ok, or a null locus otherwise.
 *
 * mergeclause_list is a List of RestrictInfo.  Its members are
 * the equijoin predicates between the outer and inner rel.  
 * It comes from select_mergejoin_clauses() in joinpath.c.
 */
CdbPathLocus
cdbpath_motion_for_join(Query  *root,
                        Path  **outer_path,         /* INOUT */
                        Path  **inner_path,         /* INOUT */
                        List   *mergeclause_list,   
                        List   *outer_pathkeys,
                        List   *inner_pathkeys,
                        bool    must_preserve_order)
{
    Path           *outer = *outer_path;
    Path           *inner = *inner_path;
    CdbPathLocus    outer_move_to;
    CdbPathLocus    inner_move_to;

    CdbPathLocus_MakeNull(&outer_move_to);  
    CdbPathLocus_MakeNull(&inner_move_to);

    /* Motion not needed if either rel is everywhere (e.g. a constant). */
    if (CdbPathLocus_IsGeneral(outer->locus))
        return inner->locus;
    else if (CdbPathLocus_IsGeneral(inner->locus))
        return outer->locus;

    /* Join must be done on qDisp if either rel is on qDisp. */
    else if (CdbPathLocus_IsRoot(outer->locus))
        inner_move_to = outer->locus;
    else if (CdbPathLocus_IsRoot(inner->locus))
        outer_move_to = inner->locus;   /* can merge to preserve ordering */

    /* Motion not needed if either rel is replicated. */
    else if (CdbPathLocus_IsReplicated(outer->locus))
        return inner->locus;
    else if (CdbPathLocus_IsReplicated(inner->locus))
        return outer->locus;

    /* No motion if partitioned alike and joining on the partitioning keys. */
    else if (cdbpath_match_preds_to_both_partkeys(root, mergeclause_list, 
                                                  outer->locus, inner->locus))
        return cdbpathlocus_join(outer->locus, inner->locus);

    /* Replicate or redistribute one rel or both.  First get rel sizes. */
    else
    {
        double          outer_rows = IsA(outer, UniquePath) 
                                     ? ((UniquePath *)outer)->rows
                                     : outer->parent->rows;
        double          outer_bytes = outer_rows * outer->parent->width;
        double          inner_rows = IsA(inner, UniquePath) 
                                     ? ((UniquePath *)inner)->rows
                                     : inner->parent->rows;
        double          inner_bytes = inner_rows * inner->parent->width;
        double          large_bytes;
        double          small_bytes;
        Path           *large;
        Path           *small;
        CdbPathLocus    large_move_to;
        CdbPathLocus    small_move_to;
        bool            large_preserve_order;
        bool            small_preserve_order;

        CdbPathLocus_MakeNull(&large_move_to);  
        CdbPathLocus_MakeNull(&small_move_to);

        /* Which rel is bigger? */
        if (outer_bytes < inner_bytes)
        {
            large = inner;
            small = outer;
            large_bytes = inner_bytes;
            small_bytes = outer_bytes;
            large_preserve_order = must_preserve_order && inner_pathkeys;
            small_preserve_order = must_preserve_order && outer_pathkeys;
        }
        else
        {
            large = outer;
            small = inner;
            large_bytes = outer_bytes;
            small_bytes = inner_bytes;
            large_preserve_order = must_preserve_order && outer_pathkeys;
            small_preserve_order = must_preserve_order && inner_pathkeys;
        }
        
        /* If joining on larger rel's partitioning key, redistribute smaller. */
        if (!small_preserve_order &&
            cdbpath_match_preds_to_partkey(root, 
                                           mergeclause_list, 
                                           large->locus,
                                           &small_move_to))    /* OUT */
        {}

        /* Replicate smaller rel if it appears likely to fit in memory. */
        else if (!small_preserve_order &&
                 small_bytes * CdbIdentity.numsegments < 0.5 * work_mem * 1024.0)
            CdbPathLocus_MakeReplicated(&small_move_to);

        /* If joining on smaller rel's partitioning key, redistribute larger. */
        else if (!large_preserve_order &&
                 cdbpath_match_preds_to_partkey(root, 
                                                mergeclause_list, 
                                                small->locus,
                                                &large_move_to))    /* OUT */
        {}

        /* Redistribute both rels on equijoin cols. */
        else if (!must_preserve_order &&
                 cdbpath_partkeys_from_preds(root, 
                                             mergeclause_list,
                                             large,
                                             &large_move_to,
                                             &small_move_to))
        {}

        /* Last resort if no usable equijoin preds: Move both rels to qDisp. */
        else
        {
            CdbPathLocus_MakeRoot(&large_move_to);
            CdbPathLocus_MakeRoot(&small_move_to);
        }

        if (outer == large)
        {
            outer_move_to = large_move_to;
            inner_move_to = small_move_to;
        }
        else
        {
            outer_move_to = small_move_to;
            inner_move_to = large_move_to;
        }
    }

    /* Move outer. */
    if (!CdbPathLocus_IsNull(outer_move_to))
    {
        outer = cdbpath_create_motion_path(outer, outer_pathkeys, outer_move_to);
        if (!outer)
            goto fail;          /* can't join these two */
        if (must_preserve_order &&
            outer_pathkeys &&
            !outer->pathkeys)
            goto fail;
    }

    /* Move inner. */
    if (!CdbPathLocus_IsNull(inner_move_to))
    {
        inner = cdbpath_create_motion_path(inner, inner_pathkeys, inner_move_to);
        if (!inner)
            goto fail;          /* can't join these two */
        if (must_preserve_order &&
            inner_pathkeys &&
            !inner->pathkeys)
            goto fail;
    }

    *outer_path = outer;
    *inner_path = inner;
    return cdbpathlocus_join(outer->locus, inner->locus);   /* ok to join */

fail:
    CdbPathLocus_MakeNull(&outer_move_to);  
    return outer_move_to;
}                               /* cdbpath_motion_for_join */


