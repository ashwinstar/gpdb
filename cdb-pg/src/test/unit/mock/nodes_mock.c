/*
 * Auto-generated Mocking Source
 * 
 * Source: nodes/nodes.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "nodes/nodes.h"

char           *
nodeToString(const void *obj)
{

    check_expected(obj);

    return (char *) (unsigned) mock();

}

char           *
nodeToBinaryStringFast(void *obj, int *size)
{

    check_expected(obj);
    check_expected(size);
    optional_assignment(obj);
    optional_assignment(size);
    return (char *) (unsigned) mock();

}

Node           *
readNodeFromBinaryString(const char *str, int len)
{

    check_expected(str);
    check_expected(len);

    return (Node *) (unsigned) mock();

}

void           *
stringToNode(char *str)
{

    check_expected(str);
    optional_assignment(str);
    mock();

}

void           *
copyObject(const void *obj)
{

    check_expected(obj);

    mock();

}

bool 
equal(const void *a, const void *b)
{

    check_expected(a);
    check_expected(b);

    return (bool) mock();

}
