/*
 * cdbbackup.h
 *
 */

#ifndef CDBBACKUP_H
#define CDBBACKUP_H

/*
 * External declarations
 */
extern Datum cdb_backup_launch__(PG_FUNCTION_ARGS);
extern Datum cdb_restore_launch__(PG_FUNCTION_ARGS);
extern Datum cdb_read_backup_file__(PG_FUNCTION_ARGS);
extern Datum cdb_write_backup_file__(PG_FUNCTION_ARGS);

#endif   /* CDBBACKUP_H */
