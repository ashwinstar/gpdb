/* chubby_counted.h */

#ifndef CHUBBY_COUNTED_H
#define CHUBBY_COUNTED_H

struct chubby_counted{
    int count;
    int closed;
    void(*delet)(struct chubby_counted*thiz);
};

void chubby_counted_set(void*a,void*b);
void chubby_counted_close(void*a);

#endif
