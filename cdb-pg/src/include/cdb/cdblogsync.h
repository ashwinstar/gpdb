/*-------------------------------------------------------------------------
 * cdblogsync.h
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBLOGSYNC_H
#define CDBLOGSYNC_H

/* sync commands */
typedef enum
{
	SYNC_POSITION_TO_END,		/* position to WAL end */
	SYNC_XLOG,					/* xlog sync */
	SYNC_NEW_CHECKPOINT_LOC,	/* new checkpoint location */
	SYNC_CLOSE					/* close */
}	syncCommand;

extern void cdb_init_log_sync(void);
extern void cdb_sync_command(const char *);

#endif   /* CDBLOGSYNC_H */
