/*
 * Auto-generated Mocking Source
 * 
 * Source: catalog/gp_policy.h, cdb/cdbcat.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "miscadmin.h"
#include "access/catquery.h"
#include "catalog/pg_namespace.h"
#include "catalog/indexing.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "cdb/cdbcat.h"
#include "cdb/cdbrelsize.h"
#include "cdb/cdbvars.h"	/* Gp_role */
#include "catalog/pg_type.h"
#include "utils/array.h"

GpPolicy       *
GpPolicyCopy(MemoryContext mcxt, const GpPolicy * src)
{

    check_expected(mcxt);
    check_expected(src);

    return (GpPolicy *) mock();

}

bool 
GpPolicyEqual(const GpPolicy * lft, const GpPolicy * rgt)
{

    check_expected(lft);
    check_expected(rgt);

    return (bool) mock();

}

GpPolicy       *
GpPolicyFetch(MemoryContext mcxt, Oid tbloid)
{

    check_expected(mcxt);
    check_expected(tbloid);

    return (GpPolicy *) mock();

}

void 
GpPolicyStore(Oid tbloid, const GpPolicy * policy)
{

    check_expected(tbloid);
    check_expected(policy);

    mock();

}

void 
GpPolicyReplace(Oid tbloid, const GpPolicy * policy)
{

    check_expected(tbloid);
    check_expected(policy);

    mock();

}

void 
GpPolicyRemove(Oid tbloid)
{

    check_expected(tbloid);

    mock();

}

void 
checkPolicyForUniqueIndex(Relation rel, AttrNumber * indattr, int nidxatts, bool isprimary, bool has_exprs, bool has_pkey, bool has_ukey)
{

    check_expected(rel);
    check_expected(indattr);
    check_expected(nidxatts);
    check_expected(isprimary);
    check_expected(has_exprs);
    check_expected(has_pkey);
    check_expected(has_ukey);
    optional_assignment(indattr);
    mock();

}
