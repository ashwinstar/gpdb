/*-------------------------------------------------------------------------
 * mpiutils.h
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef MPIUTILS_H
#define MPIUTILS_H


/* A check to determine whether a result-code indicates an MPI error.  If it
 * does, the error-code and error-string are printed to stdout, and then
 * exit(-1) is called.
 */
extern const char *getMpiErrMsg(int rc);

/* checks the MPI rc and ereports FATAL if its not SUCCESS
 *
 */
extern void checkMpiRc( int rc, char *errMsg );

#endif /* MPIUTILS_H */
