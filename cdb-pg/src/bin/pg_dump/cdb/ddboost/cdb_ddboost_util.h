/*-------------------------------------------------------------------------
 *
 * cdb_ddboost_util.h
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDB_DDBOOST_UTIL_H
#define CDB_DDBOOST_UTIL_H


/* Holds the info of the current DD connection */
typedef struct ddboost_connection_info_struct
{
	char 			*username;
	char 			*hostname;
	char 			*storageUnitName;
	uint 			logsSize;
	ddp_severity_t 	logLevel;
} ddboost_connection_info;


/* Names of the items in the Lockbox */
#define LB_ITEM_NAME_STORAGE_UNIT_NAME 		("storageUnitName")

/* Default values for the items in the Lockbox */
#define LB_DEFAULT_VALUE_LOG_SIZE			("50")
#define LB_DEFAULT_VALUE_LOG_LEVEL  		("DEBUG")
#define LB_DEFAULT_VALUE_STORAGE_UNIT_NAME 	("GPDB")



#endif /* CDB_DDBOOST_UTIL_H */
