/*
 * Auto-generated Mocking Source
 * 
 * Source: access/persistentendxactrec.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "miscadmin.h"
#include "access/persistentendxactrec.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentfilesysobj.h"

char           *
PersistentXactObjKind_Name(PersistentEndXactObjKind objKind)
{

    check_expected(objKind);

    return (char *) mock();

}

char           *
EndXactRecKind_Name(EndXactRecKind endXactRecKind)
{

    check_expected(endXactRecKind);

    return (char *) mock();

}

bool 
EndXactRecKind_NeedsAction(EndXactRecKind endXactRecKind, PersistentEndXactFileSysAction action)
{

    check_expected(endXactRecKind);
    check_expected(action);

    return (bool) mock();

}

char           *
PersistentEndXactFileSysAction_Name(PersistentEndXactFileSysAction action)
{

    check_expected(action);

    return (char *) mock();

}

void 
PersistentEndXactRec_Init(PersistentEndXactRecObjects * objects)
{

    check_expected(objects);
    optional_assignment(objects);
    mock();

}

void 
PersistentEndXactRec_AddFileSysActionInfos(PersistentEndXactRecObjects * objects, EndXactRecKind endXactRecKind, PersistentEndXactFileSysActionInfo * fileSysActionInfos, int count)
{

    check_expected(objects);
    check_expected(endXactRecKind);
    check_expected(fileSysActionInfos);
    check_expected(count);
    optional_assignment(objects);
    optional_assignment(fileSysActionInfos);
    mock();

}

int32 
PersistentEndXactRec_FetchObjectsFromSmgr(PersistentEndXactRecObjects * objects, EndXactRecKind endXactRecKind, int16 * objectCount)
{

    check_expected(objects);
    check_expected(endXactRecKind);
    check_expected(objectCount);
    optional_assignment(objects);
    optional_assignment(objectCount);
    return (int32) mock();

}

bool 
PersistentEndXactRec_WillHaveObjectsFromSmgr(EndXactRecKind endXactRecKind)
{

    check_expected(endXactRecKind);

    return (bool) mock();

}

void 
PersistentEndXactRec_Print(char *procName, PersistentEndXactRecObjects * objects)
{

    check_expected(procName);
    check_expected(objects);
    optional_assignment(procName);
    optional_assignment(objects);
    mock();

}

int16 
PersistentEndXactRec_ObjectCount(PersistentEndXactRecObjects * objects, EndXactRecKind endXactRecKind)
{

    check_expected(objects);
    check_expected(endXactRecKind);
    optional_assignment(objects);
    return (int16) mock();

}

int32 
PersistentEndXactRec_SerializeLen(PersistentEndXactRecObjects * objects, EndXactRecKind endXactRecKind)
{

    check_expected(objects);
    check_expected(endXactRecKind);
    optional_assignment(objects);
    return (int32) mock();

}

void 
PersistentEndXactRec_Serialize(PersistentEndXactRecObjects * objects, EndXactRecKind endXactRecKind, int16 * numOfObjects, uint8 * buffer, int32 bufferLen)
{

    check_expected(objects);
    check_expected(endXactRecKind);
    check_expected(numOfObjects);
    check_expected(buffer);
    check_expected(bufferLen);
    optional_assignment(objects);
    optional_assignment(numOfObjects);
    optional_assignment(buffer);
    mock();

}

int32 
PersistentEndXactRec_DeserializeLen(uint8 * buffer, int16 numOfObjects)
{

    check_expected(buffer);
    check_expected(numOfObjects);
    optional_assignment(buffer);
    return (int32) mock();

}

void 
PersistentEndXactRec_Deserialize(uint8 * buffer, int16 numOfObjects, PersistentEndXactRecObjects * objects, uint8 ** nextData)
{

    check_expected(buffer);
    check_expected(numOfObjects);
    check_expected(objects);
    check_expected(nextData);
    optional_assignment(buffer);
    optional_assignment(objects);
    optional_assignment(nextData);
    mock();

}
