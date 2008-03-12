/* chubby_counted.c */

#include "chubby_private.h"

void
chubby_counted_set(void*_a,void*_b)
{
    struct chubby_counted**a=(struct chubby_counted**)_a;
    struct chubby_counted*b=(struct chubby_counted*)_b;
    if (*a == b)
	return;
    if (*a){
	assert((*a)->count>0);
	--(*a)->count;
	if ((*a)->count==0 && (*a)->closed)
	    (*a)->delet(*a);
    }
    *a = b;
    if (b)
	b->count++;
}

void
chubby_counted_close(void*_a)
{
    struct chubby_counted*a=(struct chubby_counted*)_a;
    a->closed = 1;
    if (a->count == 0)
	a->delet(a);
}
