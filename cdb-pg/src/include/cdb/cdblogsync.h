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
	SYNC_XLOG,					/* xlog sync */
	SYNC_CHECK,					/* verification */
	SYNC_CLOSE					/* close */
}	syncCommand;

extern void cdb_init_log_sync(void);
extern void cdb_sync_command(const char *);

#endif   /* CDBLOGSYNC_H */
