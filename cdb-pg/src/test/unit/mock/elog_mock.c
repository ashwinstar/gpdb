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

ErrorContextCallback *error_context_stack = NULL;

int errcode_for_file_access(void)
{
	return 0;
}

int
ExceptionalCondition(const char *conditionName,
					 const char *errorType,
					 const char *fileName,
					 int lineNumber)
{
    abort();
	return 0;
}

bool errstart(int elevel, const char *filename, int lineno,
		 const char *funcname, const char *domain)
{
	return false;
}

void errfinish(int dummy,...)
{

}

void elog_start(const char *filename, int lineno, const char *funcname)
{
}

int do_message(const char* fmt, va_list vl)
{
	int	i = vprintf(fmt, vl);
	printf("\n");
	return (i+1);
}

void elog_finish(int elevel, const char *fmt,...)
{
	va_list vl;
	va_start(vl,fmt);
	do_message(fmt, vl);
	va_end(vl);
}

int errmsg(const char *fmt,...)
{
	int i;
	va_list vl;
	va_start(vl,fmt);
	i = do_message(fmt, vl);
	va_end(vl);
	return i;
}

int errdetail(const char *fmt,...)
{
	int i;
	va_list vl;
	va_start(vl,fmt);
	i = do_message(fmt, vl);
	va_end(vl);
	return i;
}

int errhint(const char *fmt,...)
{
	int i;
	va_list vl;
	va_start(vl,fmt);
	i = do_message(fmt, vl);
	va_end(vl);
	return i;
}

int errcode(int sqlerrcode)
{
	return 0;
}

void elog_internalerror(const char *filename, int lineno, const char *funcname)
{
	abort();
}

int
errcontext(const char *fmt,...)
{
	return 0;
}

uint32 gp_backtrace(void **stackAddresses, uint32 maxStackDepth)
{
	return 0;
}

char *gp_stacktrace(void **stackAddresses, uint32 stackDepth)
{
	return "";
}

int
errOmitLocation(bool omitLocation)
{
    check_expected(omitLocation);
    return (int)mock();
}

int errSendAlert(bool sendAlert)
{
    check_expected(sendAlert);
    return (int)mock();
}
