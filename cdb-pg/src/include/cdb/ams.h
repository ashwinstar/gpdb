/*-------------------------------------------------------------------------
* ams.h
*	AMS interaction functions.
*
* Copyright (c) 2005, Metapa
*-------------------------------------------------------------------------
*/
#ifndef AMS_H 
#define AMS_H


extern void ams_setup(void);

extern int ams_fabric_connect(void);

/* Called by ams_cleanup to disconnect cleanly with the ams-svc process
 * Also registered with postgres to be called on process exit.
 *
 * PARAMETERS:
 *  - exit_code: The exit code that caused the disconnect to occur.
 *  - arg:
 */
extern void ams_disconnect( int exit_code, Datum arg );


extern void ams_cleanup( );

#endif /*AMS_H*/
