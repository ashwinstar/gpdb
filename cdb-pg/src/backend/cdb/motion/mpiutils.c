/*-------------------------------------------------------------------------
 * mpiutils.c
 *
 *     Utility functions to help with various common MPI tasks.
 *
 * $Id$
 *-------------------------------------------------------------------------
 */
#include <mpi.h>

#include "postgres.h"
#include "cdb/mpiutils.h"


/* Space for getMpiErrMsg to do its thing. */
static char g_mpiErrMsgBuf[MPI_MAX_ERROR_STRING];
static int g_mpiErrMsgLength;


/* A check to determine whether a result-code indicates an MPI error.  If it
 * does, the error-code and error-string are printed to stdout, and then
 * exit(-1) is called.
 */
const char *getMpiErrMsg(int rc)
{
    MPI_Error_string(rc, g_mpiErrMsgBuf, &g_mpiErrMsgLength);
    return g_mpiErrMsgBuf;
}


//TODO: turn this into a define, also make it visible so ams.c can use it
void checkMpiRc( int rc, char *errMsg )
{
    if( rc != MPI_SUCCESS)
    {
        ereport(FATAL, (errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("%s  with error: %s (rc=%08X)", errMsg, getMpiErrMsg(rc), rc )));
    }
}
