/*-------------------------------------------------------------------------
 *
 * shmutils.h
 *     
 *   
 * 
 * Copyright (c) 2005, Metapa
 *
 * IDENTIFICATION:
 *     $Id$
 *-------------------------------------------------------------------------
 */

#ifndef SHMUTILS_H
#define SHMUTILS_H

#include <sys/ipc.h>

#define SHMEM_KEY 42
#define SEM_KEY 43
#define QSIZE 64000
#define NUM_SEGS 4

/* =================================================================*/
/*                        SHARED MEMORY UTILITIES                   */
/* =================================================================*/

/** Shared Memory Utilities */

extern char * ShmemCreate( key_t shmem_key, int size );
extern char * ShmemAttach( key_t shmem_key);

extern int ShmemRemove( key_t shmem_key );

/* =================================================================*/
/*                        SEMAPHORE UTILITIES                       */
/* =================================================================*/


/** Creata a new Semaphore  */
extern int SemCreate( int semKey );


/*
 * Initializes the semaphore to the specified value. 
 *
 */
extern int SemInit( int semid, int initValue );

/*
 * Attempts to obtain a semaphore id. 
 */
extern int SemGet( int semKey);


/*
 * Decrements the value of the semaphore and blocks if 0.   
 *
 * This uses semop() system call.  negative numbers are errors.
 * 
 */
extern int SemLock( int semid );



/* 
 * Posts to the semaphore (or increments its value) specified by semid;
 *
 *
 * This uses semop() system call.  negative numbers are errors.
 * callers should at a minimum check for:
 *
 * [EINVAL] No semaphore set corresponds to semid.
 *
 */
extern int SemPost( int semid );

/* 
 * Returns the current value of the semaphore by semid.
 * 
 * This uses semctl() system call.  negative numbers are errors.
 * callers should at a minimum check for:
 *
 * [EINVAL]  No semaphore set corresponds to semid.
 * 
 * [EPERM]   The calling process's effective uid does not match the uid of the
 *           semaphore set's owner or creator.
 * 
 * 
 */
extern int SemValue( int semid );


extern int SemRemove( int semid );

#endif /*SHMUTILS_H */
