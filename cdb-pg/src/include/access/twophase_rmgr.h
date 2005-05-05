/*
 * twophase_rmgr.h
 *
 * Twophase resource managers definition
 *
 * $PostgreSQL$
 */
#ifndef TWOPHASE_RMGR_H
#define TWOPHASE_RMGR_H

/*
 * Built-in resource managers
 */
#define TWOPHASE_RM_END_ID			0
#define TWOPHASE_RM_LOCK_ID			1
#define TWOPHASE_RM_INVAL_ID		2
#define TWOPHASE_RM_SMGR_ID			3
#define TWOPHASE_RM_FLATFILES_ID   	4
#define TWOPHASE_RM_NOTIFY_ID   	5
#define TWOPHASE_RM_MAX_ID		TWOPHASE_RM_NOTIFY_ID

extern const TwoPhaseCallback recover_callbacks[];
extern const TwoPhaseCallback postcommit_callbacks[];
extern const TwoPhaseCallback postabort_callbacks[];

#endif   /* TWOPHASE_RMGR_H */
