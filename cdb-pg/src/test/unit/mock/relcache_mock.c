/*
 * Auto-generated Mocking Source
 * 
 * Source: utils/relcache.h, utils/rel.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include "access/genam.h"
#include "access/catquery.h"
#include "access/heapam.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "rewrite/rewriteDefine.h"
#include "storage/fd.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "cdb/cdbcat.h"		/* GpPolicy */
#include "cdb/cdbtm.h"
#include "cdb/cdbvars.h"	/* Gp_role */
#include "cdb/cdbmirroredflatfile.h"
#include "cdb/cdbpersistentfilesysobj.h"

Relation 
RelationIdGetRelation(Oid relationId)
{

    check_expected(relationId);

    return (Relation) mock();

}

void 
RelationClose(Relation relation)
{

    check_expected(relation);

    mock();

}

struct GpPolicy *
RelationGetPartitioningKey(Relation relation)
{

    check_expected(relation);

    mock();

}

List           *
RelationGetIndexList(Relation relation)
{

    check_expected(relation);

    return (List *) mock();

}

Oid 
RelationGetOidIndex(Relation relation)
{

    check_expected(relation);

    return (Oid) mock();

}

List           *
RelationGetIndexExpressions(Relation relation)
{

    check_expected(relation);

    return (List *) mock();

}

List           *
RelationGetIndexPredicate(Relation relation)
{

    check_expected(relation);

    return (List *) mock();

}

void 
RelationSetIndexList(Relation relation, List * indexIds, Oid oidIndex)
{

    check_expected(relation);
    check_expected(indexIds);
    check_expected(oidIndex);
    optional_assignment(indexIds);
    mock();

}

void 
RelationInitIndexAccessInfo(Relation relation)
{

    check_expected(relation);

    mock();

}

void 
GpRelationNodeBeginScan(Relation gp_relation_node, Oid relationId, Oid relfilenode, GpRelationNodeScan * gpRelationNodeScan)
{

    check_expected(gp_relation_node);
    check_expected(relationId);
    check_expected(relfilenode);
    check_expected(gpRelationNodeScan);
    optional_assignment(gpRelationNodeScan);
    mock();

}

HeapTuple 
GpRelationNodeGetNext(GpRelationNodeScan * gpRelationNodeScan, int32 * segmentFileNum, ItemPointer persistentTid, int64 * persistentSerialNum)
{

    check_expected(gpRelationNodeScan);
    check_expected(segmentFileNum);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(gpRelationNodeScan);
    optional_assignment(segmentFileNum);
    optional_assignment(persistentSerialNum);
    return (HeapTuple) mock();

}

void 
GpRelationNodeEndScan(GpRelationNodeScan * gpRelationNodeScan)
{

    check_expected(gpRelationNodeScan);
    optional_assignment(gpRelationNodeScan);
    mock();

}

HeapTuple 
ScanGpRelationNodeTuple(Relation gp_relation_node, Oid relationNode, int32 segmentFileNum)
{

    check_expected(gp_relation_node);
    check_expected(relationNode);
    check_expected(segmentFileNum);

    return (HeapTuple) mock();

}

HeapTuple 
FetchGpRelationNodeTuple(Relation gp_relation_node, Oid relationNode, int32 segmentFileNum, ItemPointer persistentTid, int64 * persistentSerialNum)
{

    check_expected(gp_relation_node);
    check_expected(relationNode);
    check_expected(segmentFileNum);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(persistentSerialNum);
    return (HeapTuple) mock();

}

bool 
ReadGpRelationNode(Oid relfilenode, int32 segmentFileNum, ItemPointer persistentTid, int64 * persistentSerialNum)
{

    check_expected(relfilenode);
    check_expected(segmentFileNum);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(persistentSerialNum);
    return (bool) mock();

}

void 
RelationFetchSegFile0GpRelationNode(Relation relation)
{

    check_expected(relation);

    mock();

}

void 
RelationFetchGpRelationNodeForXLog_Index(Relation relation)
{

    check_expected(relation);

    mock();

}

void 
RelationCacheInitialize(void)
{



    mock();

}

void 
RelationCacheInitializePhase2(void)
{



    mock();

}

Relation 
RelationBuildLocalRelation(const char *relname, Oid relnamespace, TupleDesc tupDesc, Oid relid, Oid reltablespace, char relkind, bool shared_relation)
{

    check_expected(relname);
    check_expected(relnamespace);
    check_expected(tupDesc);
    check_expected(relid);
    check_expected(reltablespace);
    check_expected(relkind);
    check_expected(shared_relation);

    return (Relation) mock();

}

void 
RelationForgetRelation(Oid rid)
{

    check_expected(rid);

    mock();

}

void 
RelationCacheInvalidateEntry(Oid relationId)
{

    check_expected(relationId);

    mock();

}

void 
RelationCacheInvalidate(void)
{



    mock();

}

void 
AtEOXact_RelationCache(bool isCommit)
{

    check_expected(isCommit);

    mock();

}

void 
AtEOSubXact_RelationCache(bool isCommit, SubTransactionId mySubid, SubTransactionId parentSubid)
{

    check_expected(isCommit);
    check_expected(mySubid);
    check_expected(parentSubid);

    mock();

}

bool 
RelationIdIsInInitFile(Oid relationId)
{

    check_expected(relationId);

    return (bool) mock();

}

void 
RelationCacheInitFileInvalidate(bool beforeSend)
{

    check_expected(beforeSend);

    mock();

}

void 
RelationCacheInitFileRemove(const char *dbPath)
{

    check_expected(dbPath);

    mock();

}

void 
IndexSupportInitialize(oidvector * indclass, Oid * indexOperator, RegProcedure * indexSupport, StrategyNumber maxStrategyNumber, StrategyNumber maxSupportNumber, AttrNumber maxAttributeNumber)
{

    check_expected(indclass);
    check_expected(indexOperator);
    check_expected(indexSupport);
    check_expected(maxStrategyNumber);
    check_expected(maxSupportNumber);
    check_expected(maxAttributeNumber);
    optional_assignment(indclass);
    optional_assignment(indexOperator);
    optional_assignment(indexSupport);
    mock();

}

bool            criticalRelcachesBuilt;
void 
RelationIncrementReferenceCount(Relation rel)
{

    check_expected(rel);

    mock();

}

void 
RelationDecrementReferenceCount(Relation rel)
{

    check_expected(rel);

    mock();

}
