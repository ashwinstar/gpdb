/*-------------------------------------------------------------------------
 *
 * ams-net
 *	  Various protocol functions for ams related communication.
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */
#ifdef FRONTEND

#include "postgres_fe.h"
#include <assert.h>
#define AssertArg(expr)  \
        assert( expr )

#else
#include "postgres.h"
#endif


#include "cdb/ams-net.h"



int WriteRegisterMessage( Register *reg, uint8 *buff, int length, int* bytesWritten )
{
	int i = 0;
	int dbEntryCount = 0;
	int2 len;
	int entryLen;
	
	DbEntry *db;
	
	int j;
	
	*bytesWritten = 3;
	
	*buff = CMD_REGISTER;
	i++;
	
	//length of cdb Name
	len = strlen( reg->cdbName ) + 1;
	memcpy( buff + i, &len, 2 );
	i += 2;
	
	//printf("len: %d\n", len );	
		
	//cdb name	
	memcpy( buff + i, reg->cdbName, len );
	i += len;
	
	//group id
	memcpy( buff + i, &(reg->groupId), 4 );
	i += 4;
	
	//total number of dbIds
	memcpy( buff + i, &(reg->numDbs), 4 );
	i += 4;
	
	
	for( j=0; j < reg->numDbs; j++ )
	{
		db = reg->listOfDbs + j;
	
		len = strlen( db->hostName ) + 1;
		entryLen = 6 + len;
		
		if( length - i > entryLen )
		{
			//room for more
			
			//dbId
			memcpy( buff + i, &(db->contentId), 4 );
			i += 4;
			
			//host len
			memcpy( buff + i, &len, 2 );
			i += 2;
			
			//host name
			memcpy( buff + i, db->hostName, len );
			i += len;
			
			dbEntryCount++;
		}
		else
			break;
			
	}
		
	//update the bytes written
	*bytesWritten = i + 1;
	
	return dbEntryCount;
}

int ReadRegisterMessage( Register *reg, uint8 *buff, int length )
{
	int i = 0;
	int2 len;
	int dbEntryCount = 0;
	
	//skip the CMD_REGISTER
	i++;

	//read the name length	
	memcpy( &len, buff+i, 2 );
	i += 2;
	
	//read the name
	reg->cdbName = malloc( len );
	memcpy( reg->cdbName, buff+i, len );
	i += len;
	
	//read the group id
	memcpy( &(reg->groupId), buff+i, 4 );
	i += 4;
	
	//read number of dbIds
	memcpy( &(reg->numDbs), buff+i, 4 );
	i += 4;
	
	
	//printf("reading in dbEntries.\n");
	reg->listOfDbs = malloc( sizeof(DbEntry) * reg->numDbs );
	
	//read in some dbEntries.
	while( i != (length-1) )
	{
		//printf("i: %d  length: %d\n", i, length );
		DbEntry *db = reg->listOfDbs + dbEntryCount; 
					
		memcpy( &(db->contentId), buff+i, 4 );
		i += 4;
				
		memcpy( &len, buff+i, 2 );
		i += 2;
		
		db->hostName = malloc( len );
		memcpy( db->hostName, buff+i, len );
		i += len;		
				
		dbEntryCount++;
			
	}
	
	
	
	return dbEntryCount;
}


int WriteMapMessage( Register *reg, int dbEntryCount, uint8 *buff, int length, int *bytesWritten )
{
	int i,j;
	int2 len;	
	int entryLen;
	DbEntry *db;
	
	//write in the Message type.
	*buff = CMD_MAP;
	i = 1;
	
	//pick up where we left off.
	int start = dbEntryCount;
	
	for( j=start; j < reg->numDbs; j++ )
	{
		//printf("processing db: %d\n", j );
		
		db = reg->listOfDbs + j;
	
	    //add one since we want to copy the \0 too!
		len = strlen( db->hostName ) + 1;
		entryLen = 6 + len;
		
		//printf("entryLen: %d\n", entryLen );
		
		if( length - i > entryLen )
		{
			//room for more
			
			//dbId
			memcpy( buff + i, &(db->contentId), 4 );
			i += 4;
			
			//host len
			memcpy( buff + i, &len, 2 );
			i += 2;
			
			//host name
			memcpy( buff + i, db->hostName, len );
			i += len;
			
			dbEntryCount++;
		}
		else
			break;
			
	}
		
	//update the bytes written
	*bytesWritten = i + 1;
	
	return dbEntryCount;


	
}


int ReadMapMessage( Register *reg, int dbEntryCount, uint8 *buff, int length )
{
		
	int i;
	int2 len;
	
	//Skop the CMD_MAP
	i = 1;
	
		
	//read in some dbEntries.
	while( i != (length-1) )
	{
		DbEntry *db = reg->listOfDbs + dbEntryCount; 
					
		memcpy( &(db->contentId), buff+i, 4 );
		i += 4;
				
		memcpy( &len, buff+i, 2 );
		i += 2;
		
		db->hostName = malloc( len );
		memcpy( db->hostName, buff+i, len );
		i += len;		
				
		dbEntryCount++;
			
	}
	
	
	return dbEntryCount;	
}


int WriteRegisterResponseMessage( uint8 *buff, int length, int2 statusCode )
{
	int i;

	//write in the Message type.
	*buff = CMD_REGISTER_RESPONSE;	
	i = 1;	
	
	
	//write our status code.
	memcpy( buff + i, &statusCode, 2 );
	i += 2;
	
	//TODO: add much more good stuff here
	
	return i+1;
}


int ReadRegisterResponseMessage( uint8 *buff, int length )
{	
	int i;
	int2 responseCode;
	
	//SKIP THE MESSAGE TYPE
	i = 1;
	
	//read the response code.
	memcpy( &responseCode, buff+i, 2 );
	i =+ 2;

	return responseCode;
}


/* TODO: this assumes that all can be fit into the buffer!
 *
 */
int WriteDbHelloMessage( uint8 *buff, int length, char *cdbName, 
                            int4 groupId, int4 segId )
{
    int i;
    int2 len;
    
    //write the command type
    *buff = CMD_DB_HELLO;
    i=1;
    
    //write the cdbname length
    len = strlen(cdbName) + 1;
    memcpy( buff + i, &len, 2 );
	i += 2;
	
	//write the cdbname
	memcpy( buff + i, cdbName, len );
	i += len;
	
	//write the groupID
	memcpy( buff + i, &groupId, 4 );
	i += 4;
	
	//write the segId
	memcpy( buff + i, &segId, 4 );
	i += 4;
	
	return i+1;
    
}

void ReadDbHelloMessage( DbHello *dbHello, uint8 *buff, int length )
{
    int i;
    int2 len;        
    
    //Skip the Message type
    i = 1;
    
    //read the name length
    memcpy( &len, buff+i, 2 );
	i += 2;

    //read the cdbname
    dbHello->cdbName = malloc( len );
    memcpy( dbHello->cdbName, buff+i, len );
    i += len;
    
    //read the groupid
    memcpy( &(dbHello->groupId), buff+i, 4 );
    i += 4;
    
    //read the segid
    memcpy( &(dbHello->segId), buff+i, 4 );
    i += 4;

}


/* Create a Ping message.  Length of message is returned. */
int WritePingMessage( uint8 *buff, int length )
{
	/* write in the Message type. */
	*buff = CMD_PING;
	
	return 1;
}


