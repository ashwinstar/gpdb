/* memory.h */

#ifndef MEMORY_H
#define MEMORY_H

extern void*(*zookeeper_malloc)(size_t);
extern void(*zookeeper_free)(void*);

#endif
