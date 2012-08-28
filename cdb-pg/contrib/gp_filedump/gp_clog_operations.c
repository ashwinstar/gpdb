/*
 * gp_clog_operations.c
 *
 * This file consists of function definitions to print all the transaction Id (Xid)
 * commit status read from binary Clog file to stdout in human readable format. The
 * function definitions are on similar lines with functions defined in file "gp_clog_operations"
 * as part of clog manipulator tool. Clog.c and Slru.c (main gpdb code) are two files
 * which explain most used terminologies in this source file.
 */

#include "gp_clog_operations.h"

/*Default start xid is 0 if Clog filename is invalid*/
static unsigned int startXid = 0;

/* Dump out transaction Id commit status given the xid and the
 * byte where its commit status resides.*/
static void
print_status_to_file(unsigned int xid, char readByte)
{
    int bshift;
    int status;

    bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
    status = (readByte >> bshift) & CLOG_XACT_BITMASK;

    switch (status)
    {
        case TRANSACTION_STATUS_IN_PROGRESS :
          printf("%010u In Progress\n",xid);
          break;
        case TRANSACTION_STATUS_COMMITTED:
          printf("%010u Committed\n",xid);
          break;
        case TRANSACTION_STATUS_ABORTED:
          printf("%010u Aborted\n",xid);
          break;
        case TRANSACTION_STATUS_SUB_COMMITTED:
          printf("%010u Sub-committed\n",xid);
          break;
      }
}

/* This function validates Clog file name (inpFileName) and try to extract segment number from it.
 * This is useful to calculate starting transaction Id in the Clog file.
 * This function should be called before dumping the contents of the Clog file.
 * If Not done, starting Transaction Id in the Clog file will be considered as 0.
 */
void
gp_clog_validate_clog_filename(char *inpFileName)
{
	char	*fileName = NULL;
	int      segNum   = 0;

	Assert(inpFileName != NULL);

	/* Extract segment file number from file name */
	fileName = strrchr(inpFileName, '/');
	if (fileName)
		fileName++;
	else
		fileName = inpFileName;

	/* Currently, Clog seg file numbers can range from
	 * 0 to 0x3FF. Anything outside that will be treated as seg number 0.
	 * For details on Clog and page sizes see slru.c/clog.c*/
	if (sscanf(fileName, "%4x", &segNum) != 1)
	{
		printf("Can't recognize Clogfile name format '%s' "
				"[Will use starting Xid as 0]\n", fileName);
		segNum = 0;
	}

    if(segNum < 0 || segNum > 0x3FF)
    {
		printf("Can't recognize Clogfile name format '%s' "
				"[Will use starting Xid as 0]\n", fileName);
		segNum = 0;
    }

	/* Calculate starting transaction id in the Clog seg file. */
	startXid = segNum * CLOG_PAGES_PER_SEG * CLOG_XACTS_PER_PAGE;
}

/* This function dumps commit status of all the transaction Ids (Xid) from
 * clogFile to stdout in human readable format.
 * This function assumes a valid FILE * pointer to clog file which is opened
 * by the caller.
 */
void
gp_clog_print_all_commit_status(FILE *clogFile)
{
    char readByte;
    unsigned int xid = startXid;

    Assert(clogFile != NULL);

    /* Read a byte from the Clog file till EOF and print the
     * commit status for bits per commit status */
    while (fread(&readByte, 1, 1, clogFile) != 0)
    {
        print_status_to_file(xid++,readByte);
        print_status_to_file(xid++,readByte);
        print_status_to_file(xid++,readByte);
        print_status_to_file(xid++,readByte);
    }
}
