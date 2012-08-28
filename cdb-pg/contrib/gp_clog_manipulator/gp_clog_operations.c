/*
 * gp_clog_operations.c
 *
 * This source file consists of function definitions of all the routines to
 * update transaction Id commit status in a given Clog file. Other helping functions
 * to print return code messages , Xid commit status are also included.
 */

#include "gp_clog_operations.h"

/* Note about using assert() :-
 * -----------------------------
 * assert has been used instead of Assert(postgres.h) to avoid writing a new
 * Exception handling function. This is a small program so this change is
 * acceptable.
 */

/* Default starting and ending Xid for an input seg file is
 * 0 and maximum Xids in a complete Clog seg file 
 * */
static unsigned int startXid = 0;
static unsigned int endXid   = (CLOG_XACTS_PER_PAGE * CLOG_PAGES_PER_SEG) - 1;

static void
print_status_to_file(FILE *printFile, unsigned int xid, char readByte)
{
    int bshift;
    int status;

    bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
    status = (readByte >> bshift) & CLOG_XACT_BITMASK;

    if(!printFile)
        printFile = stdout;

    switch (status)
    {
        case TRANSACTION_STATUS_IN_PROGRESS :
          fprintf(printFile,"%010u In Progress\n",xid);
          break;
        case TRANSACTION_STATUS_COMMITTED:
          fprintf(printFile,"%010u Committed\n",xid);
          break;
        case TRANSACTION_STATUS_ABORTED:
          fprintf(printFile,"%010u Aborted\n",xid);
          break;
        case TRANSACTION_STATUS_SUB_COMMITTED:
          fprintf(printFile,"%010u Sub-committed\n",xid);
          break;
      }
}

/* Prints message corrosponding to input valid retCode. A message can be wither an error
 * or a warning.
 */
void
gp_clog_print_ret_status(int retCode)
{
    switch(retCode)
    {
        case RETERRFILEOFRDERROR:
            printf("\n>> ERROR : Reached beyond EOF. Transaction status not found."
                   "\n        OR File read error");
            break;

        case RETERRFILEOF:
            printf("\n>> ERROR : Reached beyond EOF. Transaction status not found.");
            break;

        case RETERRTOOLONG:
            printf("\n>> ERROR : Input length out of bound.");
            break;

        case RETERRINVALIDIP:
            printf(">> ERROR : Invalid input.");
            break;

        case RETOK:
            printf("\n>> Return status was okay.");
            break;

        case RETERROUTOFRANGE:
        	printf("\n>> ERROR : Input out of range.");
            break;

        case RETWARNINVALIDFILENAME:
        	printf("\n>> WARNING : Clog file name not recognized. Will use range with starting Xid to be 0.");
        	break;

        default:
            printf("\n>> ERROR : Incorrect input RET CODE.");
            break;
    }
}

/* This function validates Clog file name (inpFileName) and tries to extract segment number from it.
 * This is useful to calculate starting and ending transaction Ids in the Clog file.
 * The starting and ending Xids are returned as lowBoundXid and highBoundXid respectively.
 * If the filename is valid then correct low and high bound Xids are calculated otherwise
 * default low and high Xids are used.
 * If everything goes fine, RETOK is returned as return status else
 * RETINVALIDFILENAME is returned.
 */
int
gp_clog_validate_clog_filename(char *inpFileName, unsigned int *lowBoundXid, unsigned int *highBoundXid)
{
	char	*fileName = NULL;
	int      segNum   = 0;
	int      retCode = RETOK;

	/*Refer to the note about assert()*/
	assert(inpFileName != NULL);

	/* Extract segment file number from file name */
	fileName = strrchr(inpFileName, '/');
	if (fileName)
		fileName++;
	else
		fileName = inpFileName;

	/* Currently, Clog seg file numbers/Ids can range from
	 * 0 to 0x3FF. Anything outside that will be treated as seg number 0.
	 * For details on Clog and page sizes see slru.c/clog.c*/
	if (sscanf(fileName, "%4x", &segNum) != 1)
	{
		retCode = RETWARNINVALIDFILENAME;
		segNum = 0;
	}

    if(segNum < 0 || segNum > 0x3FF)
    {
    	retCode = RETWARNINVALIDFILENAME;
		segNum = 0;
    }

	/* Calculate starting transaction id in the Clog seg file. */
	*lowBoundXid = startXid = segNum * CLOG_PAGES_PER_SEG * CLOG_XACTS_PER_PAGE;
	*highBoundXid= endXid   = (*lowBoundXid + (CLOG_XACTS_PER_PAGE * CLOG_PAGES_PER_SEG)) - 1;

	return retCode;
}

/* Given an open valid Clog file and an an Xid , this function
 * prints the commit status of that Xid.*/
int
gp_clog_print_xid_commit_status(FILE *clogFile, unsigned int xid)
{
    unsigned int fileLocation = 0;
    char readByte;
    int ret;

    /*Refer to note about assert()*/
    assert(clogFile != NULL);

    /* Seek to correct byte location. Subtract startXid to be sure that we always
     * pretend to start with transaction Id 0*/
    fileLocation = (xid - startXid) / CLOG_XACTS_PER_BYTE;
    ret = fseek (clogFile,fileLocation,SEEK_SET);
    if (ret)
    	/*Assumption : Clog file will have atleast 1 (0th) transaction status*/
        return RETERRFILEOF;

    /*Read from the location and print the Xid commit status*/
    if (fread (&readByte,1,1,clogFile) == 1)
    {
        printf("\n>> Current transaction commit status = ");
        print_status_to_file(NULL, xid, readByte);
    }
    else
        return RETERRFILEOFRDERROR;
    return RETOK;
}

/* Updates commit status of an input Xid (xid) to status (statusToBeSet).
 * Clog file (clogFile) has to be opened by the caller and should be valid.
 * Status to be set can have following values 0(IN_PRG),1(COMT),2(ABRT),3(SUB_COMT)
 */
int
gp_clog_update_xid_commit_status(FILE *clogFile, unsigned int xid, int statusToBeSet)
{
    unsigned int fileLocation = 0;
    char readByte, byteToBeWritten;
    int ret;
    int nThBitIndex, bitToBeSet;
    int idx;

    /*Refer to the note about assert()*/
    assert(clogFile != NULL);
    assert(statusToBeSet >=0 && statusToBeSet <= 3);

    /* Seek to correct byte location. Subtract startXid to be sure that we always
     * pretend to start with transaction Id 0*/
    fileLocation = (xid - startXid) / CLOG_XACTS_PER_BYTE;
    ret = fseek (clogFile,fileLocation,SEEK_SET);
    if (ret)
	    /*Assumption : Clog file will have atleast 1 (0th) transaction status*/
        return RETERRFILEOF;

    /*Read a byte from the location */
    if (fread (&readByte,1,1,clogFile) != 1)
            return RETERRFILEOFRDERROR;

    /*Seek back to start of the byte as we want to overwrite from there itself*/
    ret = fseek (clogFile,fileLocation,SEEK_SET);
    if (ret)
	    /*Assumption : Clog file will have atleast 1 (0th) transaction status*/
        return RETERRFILEOF;

    byteToBeWritten = readByte;
    nThBitIndex = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;

    for (idx=0;idx<CLOG_BITS_PER_XACT; idx++)
    {
        bitToBeSet = (statusToBeSet >> idx) & 0x1;

        if(bitToBeSet)
            byteToBeWritten = byteToBeWritten | (1 << nThBitIndex);
        else
            byteToBeWritten = byteToBeWritten & ~(1 << nThBitIndex);

        nThBitIndex++;
    }

    /*fsync is necessary to guarantee that the change resides on disk*/
    fwrite((void *)&byteToBeWritten,1,1,clogFile);
    fflush(clogFile);
    fsync(fileno(clogFile));

    printf("\n>> Updated transaction commit status is = ");
	  print_status_to_file(NULL, xid, byteToBeWritten);
    return RETOK;
}
