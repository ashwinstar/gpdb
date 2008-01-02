/*-------------------------------------------------------------------------
 *
 * varblocktest.c
 *		Simple utility for dumping PostgreSQL XLOG files.
 *
 * Usage: varblocktest [options] < input >output
 *
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>
#include <getopt_long.h>

#include "cdb/cdbvarblock.h"

#include "varblocktest.h"

// static int		binaryFd;		/* binary FD for current file */

/* command-line parameters */
static bool 		make = false; /* when true we are doing make */
static bool 		reader = false; /* when true we are executing reader */
static int			blocksize = 100 * 1024;
static bool 		verbose = false; /* when true display a lot of detail */

#define MAX_LINEBUFFER_LEN 6000 
static uint8		lineBuffer[MAX_LINEBUFFER_LEN+1];
static bool			endOfInput = false;
static int          lineLength = 0;

#define MAX_VARBLOCKTEST_LEN 204800 
static uint8		varBlockBuffer[MAX_VARBLOCKTEST_LEN];

#define MAX_VARBLOCK_TEMPSPACE_LEN 4098 
static uint8 tempSpace [MAX_VARBLOCK_TEMPSPACE_LEN];

static uint8		tempLineBuffer[MAX_LINEBUFFER_LEN+1];

static void
doReadLine(void)
{
	int c = '*';
	int n = 0;

	while (true)
	{
		c = getc(stdin);
		if (c == EOF)
		{
			endOfInput = true;
			break;
		}
		if (c == '\n')
			break;
		if (n >= MAX_LINEBUFFER_LEN)
		{
			fprintf(stderr, "Line too long %d",n);
			exit(1);
		}
		lineBuffer[n] = c;
		n++;
	}
	lineBuffer[n] = '\0';
	lineLength = n;
}

static void
doPrintVarBlock(int bufferLen)
{
    VarBlockReader  myVarBlockReader;
	int itemCount;
    uint8 *itemPtr;
	int itemLen;
	int readerItemCount;

    VarBlockReaderInit(
        &myVarBlockReader,
        varBlockBuffer,
        bufferLen);
	
	itemCount = 0;
	readerItemCount = VarBlockReaderItemCount(&myVarBlockReader);

	if (verbose)
	{
		fprintf(stdout,"VarBlockReader:\n");
		fprintf(stdout,"    header (%p):\n", myVarBlockReader.header);
		fprintf(stdout,"        offsetsAreSmall %s:\n", (myVarBlockReader.header->offsetsAreSmall ? "true" : "false"));
		fprintf(stdout,"        version %d:\n", myVarBlockReader.header->version);
		fprintf(stdout,"        itemLenSum %d:\n", myVarBlockReader.header->itemLenSum);
		fprintf(stdout,"        itemCount %d:\n", myVarBlockReader.header->itemCount);
		fprintf(stdout,"    bufferLen %d:\n", myVarBlockReader.bufferLen);
		fprintf(stdout,"    nextIndex %d:\n", myVarBlockReader.nextIndex);
		fprintf(stdout,"    firstOffsetPtr %p:\n", myVarBlockReader.firstOffsetPtr);
		fprintf(stdout,"    nextItemPtr %p:\n", myVarBlockReader.nextItemPtr);
	}

	while (true)
	{
		itemPtr = VarBlockReaderGetNextItemPtr(&myVarBlockReader,&itemLen);
		if (itemPtr == NULL)
		{
			break;
		}
		if (verbose)
		{
			fprintf(stdout, "Reader item count %d, item ptr %p, item length %d\n",
				    itemCount, itemPtr, itemLen);
		}
		if (itemLen > 0)
		{
			memcpy(tempLineBuffer,itemPtr,itemLen);
			tempLineBuffer[itemLen] = '\0';
			if (verbose)
			{
				fprintf(stdout, "Reader: '%s'\n", tempLineBuffer);
			}
			else
			{
				fprintf(stdout, "%s\n", tempLineBuffer);
			}
		}
		itemCount++;
	}
	if (readerItemCount != itemCount)
	{
		fprintf(stderr, "Reader count %d, found %d items\n",readerItemCount,itemCount);
		exit(1);
	}
}
	
static void
doMake(char* fname)
{
    VarBlockMaker  myVarBlockMaker;
	int itemCount;
    uint8 *itemPtr;
	int actualItemCount;
	int bufferLen;

    VarBlockMakerInit(
        &myVarBlockMaker,
        varBlockBuffer,
        blocksize,
        tempSpace,
        MAX_VARBLOCK_TEMPSPACE_LEN);
	itemCount = 0;
	
	while (true)
	{
		if (verbose)
		{
			fprintf(stdout,"VarBlockMaker:\n");
			fprintf(stdout,"    header (%p):\n", myVarBlockMaker.header);
			fprintf(stdout,"        offsetsAreSmall %s:\n", (myVarBlockMaker.header->offsetsAreSmall ? "true" : "false"));
			fprintf(stdout,"        version %d:\n", myVarBlockMaker.header->version);
			fprintf(stdout,"    maxBufferLen %d:\n", myVarBlockMaker.maxBufferLen);
			fprintf(stdout,"    currentItemLenSum %d:\n", myVarBlockMaker.currentItemLenSum);
			fprintf(stdout,"    nextItemPtr %p:\n", myVarBlockMaker.nextItemPtr);
			fprintf(stdout,"    currentItemCount %d:\n", myVarBlockMaker.currentItemCount);
			fprintf(stdout,"    maxItemCount %d:\n", myVarBlockMaker.maxItemCount);
		}
	
		doReadLine();
		if (!endOfInput || lineLength > 0)
		{
			if (verbose)
			{
				fprintf(stdout, "Line: '%s', length %d\n", lineBuffer, lineLength);
			}
			itemPtr = VarBlockMakerGetNextItemPtr(&myVarBlockMaker,lineLength);
			if (itemPtr == NULL)
			{
				actualItemCount = VarBlockMakerItemCount(&myVarBlockMaker);
				Assert(actualItemCount == itemCount);
				if (itemCount == 0)
				{
					fprintf(stderr, "Item too long (#1)\n");
					exit(1);
				}
				bufferLen = VarBlockMakerFinish(&myVarBlockMaker);	
				if (verbose)
				{
					fprintf(stdout, 
						    "Finished VarBlock (length = %d, item count = %d)\n",
						    bufferLen, itemCount);
				}
				doPrintVarBlock(bufferLen);
				
			    VarBlockMakerReset(&myVarBlockMaker);
				itemCount = 0;
				itemPtr = VarBlockMakerGetNextItemPtr(&myVarBlockMaker,lineLength);
				if (itemPtr == NULL)
				{
					fprintf(stderr, "Item too long (#2)\n");
					exit(1);
				}
			}

			itemCount++;

			Assert(itemPtr != NULL);
			if (verbose)
			{
				fprintf(stdout, 
					    "Copy into pointer %p\n",
					    itemPtr);
			}
			
			if (lineLength > 0)
				memcpy(itemPtr,lineBuffer,lineLength);
				
		}

		if (endOfInput)
			break;
	}

	actualItemCount = VarBlockMakerItemCount(&myVarBlockMaker);
	Assert(actualItemCount == itemCount);
	if (itemCount > 0)
	{
		bufferLen = VarBlockMakerFinish(&myVarBlockMaker);	
		if (verbose)
		{
			fprintf(stdout, 
				    "Finished final VarBlock (length = %d, item count = %d)\n",
				    bufferLen, itemCount);
		}
		doPrintVarBlock(bufferLen);
	}
}

/*
static void
doReader(char* fname)
{
	fprintf(stderr, "Not implemented yet");
	exit(1);
}
*/

/* 
 * Exit closing files
 */
static void
exit_gracefuly(int status)
{
//	close(binaryFd);
	exit(status);
}


static void
help(void)
{
	printf("varblocktest ... \n\n");
	printf("Usage:\n");
	printf("  varblocktest [OPTION]... \n");
	printf("\nOptions controlling the output content:\n");
	printf("  -m, --make                Take standard input and create a file with VarBlocks.\n"); 
	printf("  -r, --reader              Read the file and output the items to standard output.\n");
	printf("  -b, --blocksize           The VarBlock size in bytes.\n");
	printf("  -k, --kilobytes           The VarBlock size in kilobytes.\n");
	printf("  -v, --verbose             Display detailed information.\n");
	exit(0);
}

int
main(int argc, char** argv)
{
	int	c, optindex;

	static struct option long_options[] = {
		{"make", no_argument, NULL, 'm'},
		{"reader", no_argument, NULL, 'r'},
		{"blocksize", required_argument, NULL, 'b'},
		{"kilobytes", required_argument, NULL, 'k'},
		{"verbose", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, '?'},
		{NULL, 0, NULL, 0}
	};
	
	if (argc == 1 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-?"))
		help();

	while ((c = getopt_long(argc, argv, "mrbk",
							long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'm':			/* make */
				make = true;
				break;
			case 'r':			
				reader = true;	/* reader */
				break;
			case 'b':			/* blocksize */
				blocksize = atoi(optarg);
				break;
			case 'k':			/* blocksize in kilobytes */
				blocksize = 1024 * atoi(optarg);
				break;
			case 'v':			
				verbose = true;	/* verbose */
				break;
			default:
				fprintf(stderr, "Try \"varblocktest --help\" for more information.\n");
				exit(1);
		}
	}

	if (blocksize > MAX_VARBLOCKTEST_LEN)
	{
		fprintf(stderr, "Blocksize %d greater than maximum.\n", blocksize);
		exit(1);
	}
		
	if (make && reader)
	{
		fprintf(stderr, "Specify make or reader, but not both.\n");
		exit(1);
	}
		
	if (make)
	{

/*
		char *fname;
		
		if (optind >= argc)
		{
			fprintf(stderr, "Missing file specification.\n");
			exit(1);
		}
		
		fname = argv[optind];
		binaryFd = open(fname, O_WRONLY | PG_BINARY | O_CREAT, 0);

		if (binaryFd < 0)
		{
			perror(fname);
			exit(1);
		}

*/
		doMake(NULL);
		exit_gracefuly(0);
	}

/*
	if (reader)
	{
		char *fname;
		
		if (optind >= argc)
		{
			fprintf(stderr, "Missing file specification.\n");
			exit(1);
		}
		
		fname = argv[optind];
		binaryFd = open(fname, O_RDONLY | PG_BINARY, 0);

		if (binaryFd < 0)
		{
			perror(fname);
			exit(1);
		}
		doReader(fname);
		exit_gracefuly(0);
	}
*/
	exit_gracefuly(0);
	
	/* just to avoid a warning */
	return 0;
}

/*
 * Routines needed if headers were configured for ASSERT
 */
#ifndef assert_enabled
bool		assert_enabled = true;
#endif

int
ExceptionalCondition(const char *conditionName,
					 const char *errorType,
					 const char *fileName,
					 int         lineNumber)
{
	fprintf(stderr, "TRAP: %s(\"%s\", File: \"%s\", Line: %d)\n",
			errorType, conditionName,
			fileName, lineNumber);

	abort();
	return 0;
}
