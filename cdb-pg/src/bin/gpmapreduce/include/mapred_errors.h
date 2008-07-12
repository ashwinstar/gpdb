
#ifndef MAPRED_ERRORS_H
#define MAPRED_ERRORS_H

/* Error codes used by the mapreduce driver. */


/* Trying to raise this would cause weird things to happen, don't do it. */
#define NO_ERROR  0

/* Internal errors */
#define MAPRED_PARSE_INTERNAL      1    

/* User errors */
#define MAPRED_PARSE_ERROR       100

/* Other errors */
#define MAPRED_SQL_ERROR         200

#endif
