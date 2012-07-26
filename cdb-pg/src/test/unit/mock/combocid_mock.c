/*
 * Auto-generated Mocking Source
 * 
 * Source: utils/combocid.h, access/htup.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/htup.h"
#include "access/xact.h"
#include "cdb/cdbvars.h"
#include "utils/combocid.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/tqual.h"
#include "cdb/cdbdtxcontextinfo.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "access/twophase.h"	/* max_prepared_xacts */
#include "storage/buffile.h"

CommandId 
HeapTupleHeaderGetCmin(HeapTupleHeader tup)
{

    check_expected(tup);

    return (CommandId) mock();

}

CommandId 
HeapTupleHeaderGetCmax(HeapTupleHeader tup)
{

    check_expected(tup);

    return (CommandId) mock();

}

void 
HeapTupleHeaderAdjustCmax(HeapTupleHeader tup, CommandId * cmax, bool * iscombo)
{

    check_expected(tup);
    check_expected(cmax);
    check_expected(iscombo);
    optional_assignment(cmax);
    optional_assignment(iscombo);
    mock();

}

volatile ComboCidKey comboCids;
volatile int    usedComboCids;
volatile int    sizeComboCids;
void 
AtEOXact_ComboCid(void)
{



    mock();

}
