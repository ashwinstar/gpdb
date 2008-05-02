/* jute.c */

#include "chubby_private.h"

void
deallocate_String(char**s)
{
    if (*s){
        chubby_free(*s);
	*s = NULL;
    }
}

void
deallocate_Id(struct Id*v)
{
    deallocate_String(&v->scheme);
    deallocate_String(&v->id);
}

void
deallocate_ACL(struct ACL*v)
{
    deallocate_Id(&v->id);
}

int
deallocate_String_vector(struct String_vector*v)
{
    if (v->data){
	int i;
	for (i=0; i<v->count; i++)
	    if (v->data[i])
		deallocate_String(v->data+i);
	chubby_free(v->data);
	v->data = NULL;
    }
    return 0;
}

int
deallocate_ACL_vector(struct ACL_vector *v)
{
    if (v->data){
	int i;
	for (i=0; i<v->count; i++)
	    deallocate_ACL(v->data+i);
	chubby_free(v->data);
	v->data = NULL;
    }
    return 0;
}
