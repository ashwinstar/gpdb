/*-------------------------------------------------------------------------
 *
 * access/exttableprotocol.h
 *	  Declarations for External Table Protocol functions
 *
 * Copyright (c) 2010, EMC corporation
 *-------------------------------------------------------------------------
 */

#ifndef EXTPROTOCOL_H
#define EXTPROTOCOL_H

#include "nodes/nodes.h"
#include "utils/rel.h"

/*
 * ExtProtocolData is the node type that is passed as fmgr "context" info
 * when a function is called by the External Table protocol manager.
 */
typedef struct ExtProtocolData
{
	NodeTag			type;                 /* see T_ExtProtocolData */
	Relation    	prot_relation;
	char*			prot_url;
	char*			prot_databuf;
	int				prot_maxbytes;
	void*			prot_user_ctx;
	bool			prot_last_call;
		
} ExtProtocolData;

typedef ExtProtocolData *ExtProtocol;

#define CALLED_AS_EXTPROTOCOL(fcinfo) \
	((fcinfo->context != NULL && IsA((fcinfo)->context, ExtProtocolData)))

#define EXTPROTOCOL_GET_URL(fcinfo)    	   (((ExtProtocolData*) fcinfo->context)->prot_url)
#define EXTPROTOCOL_GET_RELATION(fcinfo)   (((ExtProtocolData*) fcinfo->context)->prot_relation)
#define EXTPROTOCOL_GET_DATABUF(fcinfo)    (((ExtProtocolData*) fcinfo->context)->prot_databuf)
#define EXTPROTOCOL_GET_DATALEN(fcinfo)    (((ExtProtocolData*) fcinfo->context)->prot_maxbytes)
#define EXTPROTOCOL_GET_USER_CTX(fcinfo)   (((ExtProtocolData*) fcinfo->context)->prot_user_ctx)
#define EXTPROTOCOL_IS_LAST_CALL(fcinfo)   (((ExtProtocolData*) fcinfo->context)->prot_last_call)

#define EXTPROTOCOL_SET_LAST_CALL(fcinfo)  (((ExtProtocolData*) fcinfo->context)->prot_last_call = true)
#define EXTPROTOCOL_SET_USER_CTX(fcinfo, p) \
	(((ExtProtocolData*) fcinfo->context)->prot_user_ctx = p)


#endif /* EXTPROTOCOL_H */
