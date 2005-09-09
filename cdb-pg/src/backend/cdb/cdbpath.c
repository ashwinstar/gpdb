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
 *
 *    'pathkeys' must specify an ordering equal to or weaker than the
 *    subpath's existing ordering.
 *
 *    If no motion is needed, the caller's subpath is returned unchanged.
 *    Else if require_existing_order is true, NULL is returned if the
 *      motion would not preserve an ordering at least as strong as the
 *      specified ordering; also NULL is returned if pathkeys is NIL
 *      meaning the caller is just checking and doesn't want to add motion.
 *    Else a CdbMotionPath is returned having the either specified pathkeys
 *      (if given and the motion uses Merge Receive), or the pathkeys
 *      of the original subpath (if the motion is one-to-one), or no
 *      pathkeys otherwise (the usual case).
 */
Path *
cdbpath_create_motion_path(Query           *root,
                           Path            *subpath,
                           List            *pathkeys,
                           bool             require_existing_order,
                           CdbPathLocus     locus)
{
    CdbMotionPath      *pathnode;
    Cost                cost_per_row;
    double              rows;
    bool                hashToSelfOnly = false;

    /* No motion needed if subpath locus is exactly as requested. */
    if (CdbPathLocus_IsEqual(subpath->locus, locus))
        return subpath;

    /* Moving subpath output to qDisp? */
    if (CdbPathLocus_IsRoot(locus))
    {
        /* No motion needed if subpath can run anywhere giving same output. */
        if (CdbPathLocus_IsGeneral(subpath->locus))
            return subpath;

        /* replicated-->root would send redundant copies to the qDisp. */
        if (CdbPathLocus_IsReplicated(subpath->locus))
            return NULL;

        /* hashed-->root */
        Assert(CdbPathLocus_IsHashed(subpath->locus) ||
               CdbPathLocus_IsHashedOJ(subpath->locus));

        /* Fail if caller wants no motion. */
        if (require_existing_order &&
            !pathkeys)
            return NULL;
    }

    /* Does subpath produce same multiset of rows on every qExec of its gang? */
    else if (CdbPathLocus_IsGeneral(subpath->locus) ||
             CdbPathLocus_IsReplicated(subpath->locus))
    {
        /* No motion needed if caller wants replicated data. */
        if (CdbPathLocus_IsReplicated(locus))
            return subpath;

        /* general-->hashed or replicated-->hashed */
        Assert(CdbPathLocus_IsHashed(locus) ||
               CdbPathLocus_IsHashedOJ(locus));

        /* Fail if caller wants no motion. */
        if (require_existing_order &&
            !pathkeys)
            return NULL;

        /* Since the motion is 1-to-1, the rows remain in the same order. */
        pathkeys = subpath->pathkeys;

        /* Tell each qExec to filter on the hash key, passing rows that
         * belong to its own partition and discarding rows that belong
         * to other partitions.  Rows don't move (not far, anyway).
         */
        hashToSelfOnly = true;
    }

    /* Moving subpath output to a gang of qExecs. */
    else
    {
        /* qDisp can't send tuples to qExecs. */
        if (CdbPathLocus_IsRoot(subpath->locus))
            return NULL;

        Assert(CdbPathLocus_IsHashed(subpath->locus) ||
               CdbPathLocus_IsHashedOJ(subpath->locus));

        /* hashed-->hashed */
        if (CdbPathLocus_IsHashed(locus) ||
            CdbPathLocus_IsHashedOJ(locus))
        {
            /* No motion if subpath hash key matches caller's request. */
            if (cdbpathlocus_compare(CdbPathLocus_Comparison_Equal, subpath->locus, locus))
                return subpath;
        }

        /* hashed-->replicated */
        else
            Assert(CdbPathLocus_IsReplicated(locus));

        /* Fail if caller insists on ordered result or no motion. */
        if (require_existing_order)
            return NULL;

        /* Output streams lose any ordering they had.
         * qExecs aren't allowed to merge sorted streams (for now).
         */
        pathkeys = NIL;
    }

    /* Don't materialize before motion. */
    if (IsA(subpath, MaterialPath))
        subpath = ((MaterialPath *)subpath)->subpath;

    /* Materialize before motion if needed to avoid deadlock hazard.
     * Tell Material operator to read and store all input tuples
     * before yielding first output tuple.
     * Execution might deadlock if a Merge Receive is fed by a lower
     * Motion operator, unless the two are decoupled by an intervening
     * slackening operator such as Hash or Sort.
     * CDB TODO: A less heavy-handed solution is needed.
     */
    if (pathkeys &&
        !hashToSelfOnly &&
        subpath->motionHazard)
    {
        MaterialPath *mtlpath = create_material_path(subpath->parent, subpath);
        mtlpath->cdb_strict = true;
        mtlpath->path.motionHazard = false;
        subpath = (Path *)mtlpath;
    }

    /* Create CdbMotionPath node. */
    pathnode = makeNode(CdbMotionPath);
    pathnode->path.pathtype = T_Motion;
    pathnode->path.parent = subpath->parent;
    pathnode->path.locus = locus;
    pathnode->path.pathkeys = pathkeys;
    pathnode->subpath = subpath;
    pathnode->hashToSelfOnly = hashToSelfOnly;

    /* Cost of motion */
    cost_per_row = (cdb_motion_tuple_cost > 0.0)
                    ? cdb_motion_tuple_cost
                    : 2.0 * cpu_tuple_cost;
    rows = cdbpath_rows((Path *)pathnode);

    pathnode->path.total_cost = rows * cost_per_row + subpath->total_cost;
    pathnode->path.startup_cost = subpath->startup_cost;
    pathnode->path.memory = subpath->memory;

    /* Tell operators above us that slack may be needed for deadlock safety. */
    pathnode->path.motionHazard = true;

    return (Path *)pathnode;
}                               /* cdbpath_create_motion_path */


/*
 * cdbpath_partkeyitem_eq_constant
 */
static __inline__ bool
cdbpath_partkeyitem_eq_constant(CdbLocusType   locustype,
                                 List          *partkeyitem)
{
    if (locustype == CdbLocusType_Hashed)
        return CdbPathkeyEqualsConstant(partkeyitem);
    else if (locustype == CdbLocusType_HashedOJ)
    {
        ListCell   *cell;
        foreach(cell, partkeyitem)
        {
            List   *pathkey = (List*)lfirst(cell);
            if (CdbPathkeyEqualsConstant(pathkey))
               return true;
        }
    }
    else
        Assert(false);
    return false;
}                               /* cdbpath_partkeyitem_eq_constant */


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

    /* Get current item of partkey. */
    sublist = (List *)lfirst(partkeycell);

    /* Is there a "<partkey item> = <constant expr>" predicate?
     *
     *  If table T is distributed on cols (C,D,E) and query contains preds
     *          T.C = U.A AND T.D = <constant expr> AND T.E = U.B
     *  then we would like to report a match and return the colocus
     *          (U.A, <constant expr>, U.B)
     *  so the caller can join T and U by redistributing only U.
     *  (Note that "T.D = <constant expr>" won't be in the mergeclause_list
     *  because it isn't a join pred.)
     */
    if (cdbpath_partkeyitem_eq_constant(ctx->locus.locustype, sublist))
        copathkey = sublist;

    /* Look for an equijoin comparison to the partkey item. */
    else
    {
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

        /* Fail if didn't find a match for this partkey item. */
        if (!copathkey)
            return false;
    }

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
 * Returns true if an equijoin predicate is found in the mergeclause_list
 * for each item of the locus's partitioning key.
 *
 * (Also, a partkey item that is equal to a constant is treated as a match.)
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
                        JoinType    jointype,           /* JOIN_INNER, JOIN_FULL, JOIN_LEFT, JOIN_RIGHT */
                        Path      **outer_path,         /* INOUT */
                        Path      **inner_path,         /* INOUT */
                        List       *mergeclause_list,   /* equijoin RestrictInfo list */
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

    /* Caller can specify an ordering for each source path that is
     * the same as or weaker than the path's existing ordering.
     * Caller may insist that we do not add motion that would
     * lose the specified ordering property; otherwise the given
     * ordering is preferred but not required.
     * A required NIL ordering means no motion is allowed for that path.
     */
    outer.require_existing_order = outer_require_existing_order;
    inner.require_existing_order = inner_require_existing_order;

    /* A Unique operator may be used immediately below a Join operator,
     * to remove duplicate rows from the output of an IN subquery that
     * has been pulled up into the current query's FROM clause.
     * Other uses of the Unique operator, such as DISTINCT, aren't seen
     * here; they don't occur in the Path structure at present.
     * So if a subpath has a Unique operator, we can assume that we're
     * equijoining its output columns with outputs of the other subpath;
     * and the motion we choose for the join suffices for the Unique
     * operator also.  So Unique doesn't need motion logic of its own.
     * All we have to do is lift the Unique node above the Motion node.
     */
    if (IsA(outer.path, UniquePath))
        outer.path = ((UniquePath *)outer.path)->subpath;
    if (IsA(inner.path, UniquePath))
        inner.path = ((UniquePath *)inner.path)->subpath;

    /*
     * Join must be done sequentially on qDisp if either source is on qDisp.
     *   NB: Motion to a single node - qDisp for instance - is the only motion
     *   in which we may use Merge Receive to preserve an existing ordering.
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
                 small->bytes * getCdbSegmentCount() < large->bytes)
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
                 small->bytes * getCdbSegmentCount() < large->bytes + small->bytes)
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
        outer.path = cdbpath_create_motion_path(root,
                                                outer.path,
                                                outer_pathkeys,
                                                outer.require_existing_order,
                                                outer.move_to);
        if (!outer.path)            /* fail if outer motion not feasible */
            goto fail;
   }

    /*
     * Move inner.
     */
    if (!CdbPathLocus_IsNull(inner.move_to))
    {
        inner.path = cdbpath_create_motion_path(root,
                                                inner.path,
                                                inner_pathkeys,
                                                inner.require_existing_order,
                                                inner.move_to);
        if (!inner.path)            /* fail if inner motion not feasible */
            goto fail;
    }

    /*
     * Ok to join.  Finalize outer subpath.
     */
    if (IsA(outer.path, CdbMotionPath))
    {
        /* Add Unique operator on top if original subpath was Unique. */
        if (IsA(*outer_path, UniquePath))
            outer.path = (Path *)create_unique_path(root, outer.path->parent, outer.path);

        /* Give modified subpath to caller. */
        *outer_path = outer.path;
    }

    /* Add finishing touches to inner subpath. */
    if (IsA(inner.path, CdbMotionPath))
    {
        /* Add Unique operator on top if original subpath was Unique. */
        if (IsA(*inner_path, UniquePath))
            inner.path = (Path *)create_unique_path(root, inner.path->parent, inner.path);

        /* Give modified subpath to caller. */
        *inner_path = inner.path;
    }

    /* Tell caller where the join will be done. */
    return cdbpathlocus_join(outer.path->locus, inner.path->locus);

fail:                           /* can't do this join */
    CdbPathLocus_MakeNull(&outer.move_to);
    return outer.move_to;
}                               /* cdbpath_motion_for_join */

