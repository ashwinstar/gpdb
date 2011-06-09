/*-------------------------------------------------------------------------
 *
 * extprotocolcmds.h
 *	  prototypes for extprotocolcmds.c.
 *
 *-------------------------------------------------------------------------
 */

#ifndef EXTPROTOCOLCMDS_H
#define EXTPROTOCOLCMDS_H

#include "nodes/parsenodes.h"

extern void DefineExtProtocol(List *name, List *parameters, Oid newOid);
extern void RemoveExtProtocol(List *names, DropBehavior behavior, bool missing_ok);
extern void RemoveExtProtocolById(Oid protOid);
extern void AlterExtProtocolOwner(const char *name, Oid newOwnerId);


#endif   /* EXTPROTOCOLCMDS_H */
