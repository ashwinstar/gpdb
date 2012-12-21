/*
 * Auto-generated Mocking Source
 * 
 * Source: utils/faultinjector.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <signal.h>
#include "cdb/cdbfilerep.h"
#include "cdb/cdbfilerepservice.h"
#include "cdb/cdbresynchronizechangetracking.h"
#include "storage/spin.h"
#include "storage/shmem.h"
#include "utils/faultinjector.h"
#include "utils/hsearch.h"

FaultInjectorType_e 
FaultInjectorTypeStringToEnum(char *faultTypeString)
{

    check_expected(faultTypeString);
    optional_assignment(faultTypeString);
    return (FaultInjectorType_e) mock();

}

FaultInjectorIdentifier_e 
FaultInjectorIdentifierStringToEnum(char *faultName)
{

    check_expected(faultName);
    optional_assignment(faultName);
    return (FaultInjectorIdentifier_e) mock();

}

DDLStatement_e 
FaultInjectorDDLStringToEnum(char *ddlString)
{

    check_expected(ddlString);
    optional_assignment(ddlString);
    return (DDLStatement_e) mock();

}

Size 
FaultInjector_ShmemSize(void)
{



    return (Size) mock();

}

void 
FaultInjector_ShmemInit(void)
{



    mock();

}

FaultInjectorType_e 
FaultInjector_InjectFaultIfSet(FaultInjectorIdentifier_e identifier, DDLStatement_e ddlStatement, char *databaseName, char *tableName)
{

    check_expected(identifier);
    check_expected(ddlStatement);
    check_expected(databaseName);
    check_expected(tableName);
    optional_assignment(databaseName);
    optional_assignment(tableName);
    return (FaultInjectorType_e) mock();

}

int 
FaultInjector_SetFaultInjection(FaultInjectorEntry_s * entry)
{

    check_expected(entry);
    optional_assignment(entry);
    return (int) mock();

}

bool 
FaultInjector_IsFaultInjected(FaultInjectorIdentifier_e identifier)
{

    check_expected(identifier);

    return (bool) mock();

}
