/*
 * twophase_rmgr.c
 *
 * Twophase resource managers definition
 *
 * $PostgreSQL$
 */
#include "postgres.h"

#include "access/twophase.h"
#include "access/twophase_rmgr.h"

#include "storage/lockrmgr.h"
#include "storage/smgr.h"
#include "utils/inval.h"
//  TODO FLATFILE  #include "utils/flatfiles.h"
#include "commands/async.h"

const TwoPhaseCallback recover_callbacks[TWOPHASE_RM_MAX_ID + 1] = 
{
	NULL, /* Reserved for twophase.c internal logic */
	lock_recover_record, /* Lock */
	NULL, /* Inval */
	NULL, /* smgr */
	NULL,  /* user/group file update */
	NULL /* notify/listen */
};

const TwoPhaseCallback postcommit_callbacks[TWOPHASE_RM_MAX_ID + 1] = 
{
	NULL, /* Reserved for twophase.c internal logic */
	NULL,				/* Lock */
	inval_postcommit_record,		/* Inval */
	smgr_postcommit_record,		/* smgr */
	//  TODO TWOPHASE --  flatfile_postcommit_record,	/* user/group file update */
        NULL,
	notify_postcommit_record /* notify/listen */
};

const TwoPhaseCallback postabort_callbacks[TWOPHASE_RM_MAX_ID + 1] = 
{
	NULL, /* Reserved for twophase.c internal logic */
	lock_postabort, /* Lock */
	NULL, /* Inval */
	smgr_postabort_record, /* smgr */
	NULL,  /* user/group file update */
	NULL
};
