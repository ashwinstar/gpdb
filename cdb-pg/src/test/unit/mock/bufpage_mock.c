/*
 * Manually created mocking source
 *
 * This file is not automatically updated by generate_mocks.py
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "storage/bufpage.h"

void 
PageInit(Page page, Size pageSize, Size specialSize)
{
	check_expected(page);
	check_expected(pageSize);
	check_expected(specialSize);

	mock();
}

bool 
PageHeaderIsValid(PageHeader page)
{
	check_expected(page);

	return (bool) mock();
}

Page BufferGetPage(Buffer buffer)
{
	check_expected(buffer);

	return (Page) mock();
}

OffsetNumber 
PageAddItem(Page page, Item item, Size size, OffsetNumber offsetNumber, ItemIdFlags flags)
{
	check_expected(page);
	check_expected(item);
	check_expected(size);
	check_expected(offsetNumber);
	check_expected(flags);

	return (OffsetNumber) mock();
}

Page 
PageGetTempPage(Page page, Size specialSize)
{
	check_expected(page);
	check_expected(specialSize);

	return (Page) mock();
}

void 
PageRestoreTempPage(Page tempPage, Page oldPage)
{
	check_expected(tempPage);
	check_expected(oldPage);

	mock();
}

int 
PageRepairFragmentation(Page page, OffsetNumber * unused)
{
	check_expected(page);
	check_expected(unused);
	optional_assignment(unused);
	return (int) mock();
}

Size 
PageGetFreeSpace(Page page)
{
	check_expected(page);

	return (Size) mock();
}

void 
PageIndexTupleDelete(Page page, OffsetNumber offnum)
{
	check_expected(page);
	check_expected(offnum);

	mock();
}

void 
PageIndexMultiDelete(Page page, OffsetNumber * itemnos, int nitems)
{
	check_expected(page);
	check_expected(itemnos);
	check_expected(nitems);
	optional_assignment(itemnos);
	mock();
}
