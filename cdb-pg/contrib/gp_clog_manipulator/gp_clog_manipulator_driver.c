/*
 * gp_clog_manipulator_driver.c
 * This is the driver routine for clog manipulator.
 *  Created on: Aug 4, 2012
 *      Author: tulshr
 */

#include "gp_clog_operations.h"

/*Some useful defines*/
#define XID_STR_MAX_LENGTH   10
#define FILE_ADDR_MAX_LENGTH 100
#define IN_PRG               0
#define SUB_COMT             3

static char clogFileAddr[FILE_ADDR_MAX_LENGTH];
static FILE *clogFile;

static void print_start_message();
static int input_string(char *str, int length);
static int try_again();
static int input_unsigned_int(unsigned int *number, unsigned int lowBound, unsigned int highBound);

static void print_start_message()
{
        printf("\nEMC Greenplum Database CLOG manipulator V1.0"
               "\nSupports PostgreSQL 8.2.15 (Greenplum Database main build dev)");
        printf("\nCaution : This tool should be used only by EMC Greenplum authorized personnel (Support/Development)"
               "\n          as incorrect changes to CLOG can break transaction management and thus the system. "  );
}

static
int try_again()
{
    char option[2];

    printf("\nWant to try again ? [Y|y / N|n] = ");
    if(input_string(option, 2) == RETOK)
    	if(toupper(option[0]) == (int)'Y')
    			return 1;

    return 0;
}

static int input_string(char *str, int length)
{
	assert(str && length > 0);

	fflush(stdin);
	if(fgets(str, length, stdin) != NULL)
	{
		/*Trick to avoid un-flushed characters :- Consume everything!*/
		char *newLine = strchr(str, '\n');
		if (newLine != NULL)
			*newLine = '\0';
		else
			while(fgetc(stdin) != '\n' && fgetc(stdin) != EOF);

		if(strlen(str) == 0)
			return RETERRINVALIDIP;
	}
	else
		return RETERRFILEOFRDERROR;

	return RETOK;
}

/* This function requests for an input valid unsigned int from stdin.
 * It will return appropriate return codes based on the input from the user.
 * If the return code is RETOK then the number entered by the user is valid
 * otherwise its invalid. */
static int input_unsigned_int(unsigned int *number, unsigned int lowBound, unsigned int highBound)
{
	char str[XID_STR_MAX_LENGTH + 1];
	int idx;
	int retCode;
	int power = 0;
	unsigned long long tempInt = 0;

	/*+1 is added to length for maintaining '\0'*/
	retCode = input_string(str, XID_STR_MAX_LENGTH + 1);
	if (retCode == RETOK)
	{
		    for(idx = 0;idx < XID_STR_MAX_LENGTH; idx++ )
		    {
		    	if(str[idx] == '\0')
		    		break;
		    	/*Nothing other than digits allowed*/
		    	else if(str[idx] < 48 || str[idx] > 57)
		    		return RETERRINVALIDIP;
		    }
	}
	else
		return retCode;

   power = strlen(str) - 1;
   for (idx =0; idx < strlen(str); idx++)
   {
       tempInt += (str[idx] - 48) * (pow(10, power));
       power--;
   }

   /*Check validity based on range of unsigned int*/
   if(tempInt < lowBound || tempInt > highBound)
	   return RETERROUTOFRANGE;
   else
	   *number = (unsigned int)tempInt;

   return RETOK;
}

static void clog_operations()
{
    int retCode = 0;
    unsigned int xid = 0;
    unsigned int statusToBeSet = 0;
    unsigned int lowBoundXid, highBoundXid;

    /* Using the Clog seg file name find out the seg number and thus the
     * starting transaction Id (Xid) and the last Xid in the Clog file*/
    retCode = gp_clog_validate_clog_filename(clogFileAddr,&lowBoundXid,&highBoundXid);
    if (retCode != RETOK)
    	gp_clog_print_ret_status(retCode);

    while(1)
    {
    	printf("\nEnter valid transaction ID (XID)[%u - %u] whose commit status is to be updated = ",lowBoundXid, highBoundXid);
    	retCode = input_unsigned_int(&xid, lowBoundXid, highBoundXid);
    	if (retCode == RETOK)
    	{
    		retCode = gp_clog_print_xid_commit_status(clogFile,xid);
    		if (retCode != RETOK)
    			gp_clog_print_ret_status(retCode);
    		printf("\nTransaction commit status codes : 0(IN_PRG),1(COMT),2(ABRT),3(SUB_COMT)");
    		printf("\nEnter the new status to be set to Transaction %u = ",xid);
    		retCode = input_unsigned_int(&statusToBeSet, IN_PRG, SUB_COMT);

    		if (retCode != RETOK)
    			gp_clog_print_ret_status(retCode);
    		else
    		{
    			retCode = gp_clog_update_xid_commit_status(clogFile, xid, statusToBeSet);
    			if (retCode != RETOK)
    				gp_clog_print_ret_status(retCode);
    		}
    	}
    	else
    		gp_clog_print_ret_status(retCode);

    	if(!try_again())
    		break;
    }
}

int main ()
{
    int retCode;

    while (1)
    {
        print_start_message();
        printf("\n\nEnter CLOG file address [Max 100 characters] = ");

        /*+1 is added to length for maintaining '\0'*/
        retCode = input_string(clogFileAddr, FILE_ADDR_MAX_LENGTH + 1);
        if (retCode != RETOK)
        {
        	gp_clog_print_ret_status(retCode);
        	if(try_again())
        		continue;
        	else
        		break;
        }
        else
        {
            /*Try to open the file. If no existence then error! out else continue*/
            clogFile = fopen(clogFileAddr,"r+");
            if (clogFile == NULL)
            {
                printf("\n>> ERROR: %s",strerror(errno));
                if (try_again())
                    continue;
                break;
            }
            else
            {
                clog_operations();
                fclose(clogFile);
                clogFile = NULL;
                break;
            }

        }
    }

    printf("\n>> Exiting EMC Greenplum Database CLOG manipulator.\n");
    return 0;
}
