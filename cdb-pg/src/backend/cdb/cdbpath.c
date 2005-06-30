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

#include "cdb/cdbpath.h"        /* me */

#ifdef small                    /* <socket.h> might #define small */
#undef small                    /*  but I want it for a variable name */
#endif


/* Globals */
Cost        cdb_motion_tuple_cost   = 0.0;


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

    /* NB: If data is everywhere, no motion is needed; the CdbMotionPath
     * node merely assigns the subpath a specific locus.
     */

    /* Execution might deadlock if a Merge Receive is fed by a lower
     * Motion operator, unless the two Motion operators are decoupled
     * by an intervening slackening operator such as Material, Hash or Sort.
     */
    if (!CdbPathLocus_IsGeneral(subpath->locus))
    {
        /* Materialize before motion if needed to avoid deadlock hazard. */
        if (pathkeys &&
            subpath->motionHazard)
            subpath = (Path *)create_material_path(subpath->parent, subpath);

        /* Otherwise don't materialize before motion. */
        else if (IsA(subpath, MaterialPath))
            subpath = ((MaterialPath *)subpath)->subpath;
    }

    /* Allocate CdbMotionPath node. */
    pathnode = makeNode(CdbMotionPath);
    pathnode->path.pathtype = T_Motion;
    pathnode->path.parent = subpath->parent;
    pathnode->path.locus = locus;
    pathnode->path.pathkeys = pathkeys;
    pathnode->path.isFinished = subpath->isFinished;
    pathnode->path.motionHazard = subpath->motionHazard;
    pathnode->subpath = subpath;
    pathnode->hashToSelfOnly = hashToSelfOnly;

    pathnode->path.startup_cost = subpath->startup_cost;
    pathnode->path.total_cost = subpath->total_cost;
    pathnode->path.memory = subpath->memory;

    /*
     * Cost of motion
     */
    if (!CdbPathLocus_IsGeneral(subpath->locus))
    {
        Cost    cost_per_row = (cdb_motion_tuple_cost > 0.0)
                             ? cdb_motion_tuple_cost
                             : cpu_tuple_cost;
        double  rows = cdbpath_rows(&pathnode->path);

        pathnode->path.total_cost += rows * cost_per_row;

        pathnode->path.motionHazard = true;
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

typedef struct
{
        CdbPathLocus    locus;
        CdbPathLocus    move_to;
        double          rows;
        double          bytes;
        Path           *path;
        bool            oj_preserved;
        bool            require_existing_order;
} CdbpathMfjRel;

CdbPathLocus
cdbpath_motion_for_join(Query      *root,
                        JoinType    jointype,
                        Path      **outer_path,         /* INOUT */
                        Path      **inner_path,         /* INOUT */
                        List       *mergeclause_list,
                        List       *outer_pathkeys,
                        List       *inner_pathkeys,
                        bool        outer_require_existing_order,
                        bool        inner_require_existing_order)
{
    CdbpathMfjRel   outer;
    CdbpathMfjRel   inner;

    outer.path  = *outer_path;
    inner.path  = *inner_path;
    outer.locus = outer.path->locus;
    inner.locus = inner.path->locus;
    CdbPathLocus_MakeNull(&outer.move_to);
    CdbPathLocus_MakeNull(&inner.move_to);
    outer.oj_preserved = (jointype == JOIN_FULL) || (jointype == JOIN_LEFT);
    inner.oj_preserved = (jointype == JOIN_FULL) || (jointype == JOIN_RIGHT);

    /*
     * Caller can specify an ordering for each source path that is
     * the same as or weaker than the path's existing ordering.
     * For merge join we must maintain the given ordering, if any.
     * It is preferred but not required for other join methods.
     */
    outer.require_existing_order = outer_require_existing_order;
    inner.require_existing_order = inner_require_existing_order;

    /*
     * Join must be done sequentially on qDisp if either source is on qDisp.
     * Ordering can be maintained, if requested, by using Merge Receive.
     */
    if (CdbPathLocus_IsRoot(outer.locus))
        inner.move_to = outer.locus;
    else if (CdbPathLocus_IsRoot(inner.locus))
        outer.move_to = inner.locus;

    /*
     * Motion not needed if either source is everywhere (e.g. a constant).
     */
    else if (CdbPathLocus_IsGeneral(outer.locus))
    {
        if (outer.oj_preserved &&
            !CdbPathLocus_IsGeneral(inner.locus))
        {
            CdbPathLocus_MakeRoot(&outer.move_to);
            CdbPathLocus_MakeRoot(&inner.move_to);
        }
        else
            return inner.locus;
    }
    else if (CdbPathLocus_IsGeneral(inner.locus))
    {
        if (inner.oj_preserved &&
            !CdbPathLocus_IsGeneral(outer.locus))
        {
            CdbPathLocus_MakeRoot(&outer.move_to);
            CdbPathLocus_MakeRoot(&inner.move_to);
        }
        else
            return outer.locus;
    }

    /*
     * Replicated paths shouldn't occur loose, for now.
     */
    else if (CdbPathLocus_IsReplicated(outer.locus) ||
             CdbPathLocus_IsReplicated(inner.locus))
    {
        Assert(false);
        goto fail;
    }

    /*
     * No motion if partitioned alike and joining on the partitioning keys.
     */
    else if (cdbpath_match_preds_to_both_partkeys(root, mergeclause_list,
                                                  outer.locus, inner.locus))
        return cdbpathlocus_join(outer.locus, inner.locus);

    /*
     * Redistribute or replicate one source or both.
     */
    else
    {
        CdbpathMfjRel  *large;
        CdbpathMfjRel  *small;

        /* Get rel sizes. */
        outer.rows  = cdbpath_rows(outer.path);
        outer.bytes = outer.rows * outer.path->parent->width;
        inner.rows  = cdbpath_rows(inner.path);
        inner.bytes = inner.rows * inner.path->parent->width;

        /* Which rel is bigger? */
        small = (outer.bytes < inner.bytes) ? &outer : &inner;
        large = (outer.bytes < inner.bytes) ? &inner : &outer;

        /* If joining on larger rel's partitioning key, redistribute smaller. */
        if (!small->require_existing_order &&
            cdbpath_match_preds_to_partkey(root,
                                           mergeclause_list,
                                           large->locus,
                                           &small->move_to))    /* OUT */
        {}

        /* Replicate smaller rel if cheaper than redistributing larger rel.
         * But don't replicate a rel that is to be preserved in outer join.
         */
        else if (!small->require_existing_order &&
                 !small->oj_preserved &&
                 small->bytes * CdbIdentity.numsegments < large->bytes)
            CdbPathLocus_MakeReplicated(&small->move_to);

        /* If joining on smaller rel's partitioning key, redistribute larger. */
        else if (!large->require_existing_order &&
                 cdbpath_match_preds_to_partkey(root,
                                                mergeclause_list,
                                                small->locus,
                                                &large->move_to))   /* OUT */
        {}

        /* Replicate smaller rel if cheaper than redistributing both rels. */
        else if (!small->require_existing_order &&
                 !small->oj_preserved &&
                 small->bytes * CdbIdentity.numsegments < large->bytes + small->bytes)
            CdbPathLocus_MakeReplicated(&small->move_to);

        /* Redistribute both rels on equijoin cols. */
        else if (!small->require_existing_order &&
                 !large->require_existing_order &&
                 cdbpath_partkeys_from_preds(root,
                                             mergeclause_list,
                                             large->path,
                                             &large->move_to,
                                             &small->move_to))
        {}

        /* No usable equijoin preds.  Replicate smaller rel if possible. */
        /* CDB TODO: Consider number of seg dbs per host. */
        else if (!small->require_existing_order &&
                 !small->oj_preserved)
            CdbPathLocus_MakeReplicated(&small->move_to);

        /* Last resort: Move both rels to qDisp. */
        else
        {
            CdbPathLocus_MakeRoot(&outer.move_to);
            CdbPathLocus_MakeRoot(&inner.move_to);
        }
    }

    /*
     * Move outer.
     */
    if (!CdbPathLocus_IsNull(outer.move_to))
    {
        outer.path = cdbpath_create_motion_path(outer.path,
                                                outer_pathkeys,
                                                outer.move_to);
        if (!outer.path)            /* fail if motion not feasible */
            goto fail;
        if (outer.require_existing_order &&
            outer.path->pathkeys != outer_pathkeys)
            goto fail;              /* fail if motion loses required ordering */
    }

    /*
     * Move inner.
     */
    if (!CdbPathLocus_IsNull(inner.move_to))
    {
        inner.path = cdbpath_create_motion_path(inner.path,
                                                inner_pathkeys,
                                                inner.move_to);
        if (!inner.path)
            goto fail;
        if (inner.require_existing_order &&
            inner.path->pathkeys != inner_pathkeys)
            goto fail;
    }

    /*
     * ok to join
     */
    *outer_path = outer.path;
    *inner_path = inner.path;
    return cdbpathlocus_join(outer.path->locus, inner.path->locus);

fail:                           /* can't do this join */
    CdbPathLocus_MakeNull(&outer.move_to);
    return outer.move_to;
}                               /* cdbpath_motion_for_join */


