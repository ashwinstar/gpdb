/*-------------------------------------------------------------------------
 *
 * shmutils.c
 *     
 *   
 * 
 * Copyright (c) 2005, Metapa
 *
 * IDENTIFICATION:
 *     $Id$
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sem.h>
#include <semaphore.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include "cdb/shmutils.h"


#ifndef HAVE_UNION_SEMUN
union semun
{
    int         val;
    struct semid_ds *buf;
    unsigned short *array;
};
#endif

/* =================================================================*/
/*                        SHARED MEMORY UTILITIES                   */
/* =================================================================*/



char *
ShmemCreate( key_t shmem_key, int size )
{

    int shmid;
	
	/*
	 * Locate the segment.
	 */
	if ((shmid = shmget(shmem_key, size, IPC_CREAT | IPC_EXCL | 0666)) < 0) {
		return NULL;
	}
	
	/*
	 * Now we attach the segment to our data space.
	 */
	 return (char*)shmat(shmid, NULL, 0);


}


/* Function ShmemAttach
 *
 * return value NULL indicates an error with shmget
 *
 * return value (char *) -1 indicates an error with shmat
 */
char * 
ShmemAttach( key_t shmem_key )
{
	
	int shmid;
	
	/*
	 * Locate the segment.
	 */
	if ((shmid = shmget(shmem_key, 0,  0666)) < 0) {
		return NULL;
	}
	
	/*
	 * Now we attach the segment to our data space.
	 */
	 return (char*)shmat(shmid, NULL, 0);
	 
}


int
ShmemRemove( key_t shmem_key )
{
    int shmid;
	
	/*
	 * Locate the segment.
	 */
	if ((shmid = shmget(shmem_key, 0, 0666)) < 0) {
		return NULL;
	}
	
    return shmctl(shmid, IPC_RMID, NULL );
}

/* =================================================================*/
/*                        SEMAPHORE UTILITIES                       */
/* =================================================================*/

/* Function: SemCreate
 *
 * Creates a single valued semaphore and returns a semaphore id for it.
 *
 */
int 
SemCreate( int semKey )
{
	//get our semaphore, initialize it
	return semget(semKey, 1, IPC_CREAT | IPC_EXCL | 0600);
}


/* Function: SemInit
 *
 * Sets an initial value to the semaphore.
 *
 */
int 
SemInit( int semid, int initValue )
{
    union semun sem_val;
	sem_val.val = initValue;
	
	return semctl(semid, 0, SETVAL, sem_val);
	
}


/* Function: SemGet
 *
 * obtains the semaphore id for the specified semKey. 
 * 
 */
int 
SemGet( int semKey )
{
	int semid;
	
	//get our semaphore, initialize it
	if( (semid = semget(semKey, 1, IPC_CREAT | 0600)) < 0 ){
		return -1;
	}
	
	return semid;
}


/* Function: SemLock
 *
 * 
 */ 
int 
SemLock( int semid )
{
	
	struct sembuf sops;
	
	sops.sem_op = -1;			/* decrement */
	sops.sem_flg = SEM_UNDO;	
	sops.sem_num = 0;
	
	return semop(semid, &sops, 1);
	
}


/* Function: SemPost
 *
 */
int 
SemPost( int semid )
{
	
	struct sembuf sops;
	
	sops.sem_op = 1;			/* increment */
	sops.sem_flg = SEM_UNDO;	
	sops.sem_num = 0;
	
	return semop(semid, &sops, 1);
	
}


/* Function: SemValue
 *
 */
int  SemValue( int semid )
{
	return semctl(semid, 0, GETVAL);		
}


int 
SemRemove( int semKey )
{
    int semid = SemGet( semKey);
    
    return semctl(semid, 1, IPC_RMID);
}
