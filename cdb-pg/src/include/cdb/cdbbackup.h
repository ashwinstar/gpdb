/*
 * cdbbackup.h
 *
 */

#ifndef CDBBACKUP_H
#define CDBBACKUP_H

/*
 * External declarations
 */
extern Datum mpp_backup_launch__(PG_FUNCTION_ARGS);
extern Datum mpp_restore_launch__(PG_FUNCTION_ARGS);
extern Datum mpp_read_backup_file__(PG_FUNCTION_ARGS);
extern Datum mpp_write_backup_file__(PG_FUNCTION_ARGS);

#endif   /* CDBBACKUP_H */
