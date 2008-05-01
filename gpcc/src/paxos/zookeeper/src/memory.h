/* memory.h */

#ifndef MEMORY_H
#define MEMORY_H

extern void*(*chubby_malloc)(size_t);
extern void(*chubby_free)(void*);

#endif
