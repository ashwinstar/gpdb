/*-------------------------------------------------------------------------
 *
 * cdbpathlocus.c
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cdb/cdbcat.h"         /* CdbPolicy */
#include "nodes/relation.h"     /* RelOptInfo */
#include "optimizer/pathnode.h" /* Path */
#include "optimizer/paths.h"    /* cdb_build_distribution_pathkeys() */
#include "optimizer/tlist.h"    /* tlist_member() */

#include "cdb/cdbpathlocus.h"   /* me */


/*
 * cdbpathlocus_compare
 *
 *    - Returns true if locus a and locus b have the same 'locustype' and
 *      'partkey'.
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
        {
            CdbPathLocus t = a;
            a = b;
            b = t;
        }
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
cdbpathlocus_from_baserel(struct Query         *root,
                          struct RelOptInfo    *rel)
{
    CdbPathLocus result;
    CdbPolicy   *policy = rel->cdbpolicy;

    if (policy &&
        policy->ptype == POLICYTYPE_PARTITIONED)
    {
        List *partkey = cdb_build_distribution_pathkeys(root,
                                                        rel,
                                                        policy->nattrs,
                                                        policy->attrs);
        CdbPathLocus_MakeHashed(&result, partkey);
    }
    else
        CdbPathLocus_MakeRoot(&result);

    return result;
}                               /* cdbpathlocus_from_baserel */


/*
 * cdbpathlocus_get_pathkeyitem
 *
 * Search equivalence class for an item that uses no rels outside
 * the given set, and is in the targetlist (if given).
 */
static PathKeyItem *
cdbpathlocus_get_pathkeyitem(List          *pathkey,
                             Relids         relids,
                             List          *targetlist,
                             PathKeyItem   *initial)
{
    ListCell   *pathkeycell;
    foreach(pathkeycell, pathkey)
    {
        PathKeyItem    *item = lfirst(pathkeycell);
        Assert(IsA(item, PathKeyItem));
        if (bms_is_subset(item->cdb_key_relids, relids))
        {
            if (targetlist &&
                !tlist_member(item->key, targetlist))
                continue;
            if (IsA(item->key, Var) ||
                item->cdb_num_relids == 0)
                return item;
            if (!initial ||
                item->cdb_num_relids < initial->cdb_num_relids)
                initial = item;
        }
    }
    return initial;
}                               /* cdbpathlocus_get_pathkeyitem */


/*
 * cdbpathlocus_get_partkey_exprs
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
            item = cdbpathlocus_get_pathkeyitem(pathkey, relids, targetlist, NULL);
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
                                                    targetlist,
                                                    item);
                if (item)
                {
                    if (IsA(item->key, Var) ||
                        item->cdb_num_relids == 0)
                        break;
                }
           }
        }

        /* Return NIL if distribution key column is not in the target list. */
        if (!item)
        {
            list_free(result);
            return NIL;
        }

        result = lappend(result, item->key);
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

    /* Usual case: both input rels have same locus. */
    if (CdbPathLocus_IsEqual(a, b) ||
        cdbpathlocus_compare(CdbPathLocus_Comparison_Equal, a, b))
        return a;

    /* Join will be done on qDisp if either rel is on qDisp. */
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
    {
        CdbPathLocus tmp = a;
        a = b;
        b = tmp;
    }

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

