/*-------------------------------------------------------------------------
* ams-net.h
*	AMS Network Comm functions.     
*
* Copyright (c) 2005, Metapa
*-------------------------------------------------------------------------
*/
#ifndef AMS_NET_H 
#define AMS_NET_H


/* Maximum packet-size for an AMS packet. */
#define MAX_AMS_PACKET_SIZE 65536


#define CMD_PING     'P'    /* Ping the amsd to make sure it is alive. */

#define CMD_REGISTER 'R'    /* command used for QD to register with amsd */
#define CMD_MAP      'M'    /* command used for QD to send map info that
                             * didn't fit in CMD_REGISTER. */

#define CMD_REGISTER_RESPONSE 'K'  /* response to register. */
#define CMD_DB_HELLO 'H'  /*QD/QE hello to ams-svc. */

#define CMD_QUIT     'Q'    /* Tell amsd/ams-svc to shut down. */

#define AMP_OK      1
#define AMP_ERROR   2


#define MAX_CMD_SIZE 512

/* TODO: should be dynamic and based on cdb instance name. */
#define AMSD_PUBNAME "cdb2-amsd"

/* TODO: should come from the name in the schema and be dynamic. */
#define CDB_NAME "cdb2"

typedef struct DbEntry
{
	int contentId;
	char *hostName;
} DbEntry;

typedef struct Register
{
	char *cdbName;
	int4 groupId;
	int4 numDbs;
	DbEntry *listOfDbs;

} Register;

typedef struct DbHello
{
    char *cdbName;
	int4 groupId;
    int4 segId;
} DbHello;



extern int WriteRegisterMessage( Register *reg, uint8 *buff, 
							     int length, int *bytesWritten );
extern int ReadRegisterMessage( Register *reg, uint8 *buff, int length );

extern int WriteMapMessage( Register *reg, int dbEntryCount, uint8 *buff, 
			   	 	       int length, int *bytesWritten );

extern int ReadMapMessage( Register *reg, int dbEntryCount, uint8 *buff, 
				 		   int length );
				 		   
extern int WriteRegisterResponseMessage( uint8 *buff, int length, int2 statusCode );

extern int ReadRegisterResponseMessage( uint8 *buff, int length );				 		   

extern int WriteDbHelloMessage( uint8 *buff, int length, char *cdbName, 
                            int4 groupId, int4 segId );

extern void ReadDbHelloMessage( DbHello *dbHello, uint8 *buff, int length );

extern int WritePingMessage(uint8 *buff, int length);

#endif /*AMS_NET_H*/
