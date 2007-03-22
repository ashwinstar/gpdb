/*-------------------------------------------------------------------------
 *
 * copy.h
 *	  Definitions for using the POSTGRES copy command.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/copy.h,v 1.28 2006/08/30 23:34:22 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPY_H
#define COPY_H

#include "nodes/parsenodes.h"
#include "tcop/dest.h"
#include "executor/executor.h"


/*
 * Represents the different source/dest cases we need to worry about at
 * the bottom level
 */
typedef enum CopyDest
{
	COPY_FILE,					/* to/from file */
	COPY_OLD_FE,				/* to/from frontend (2.0 protocol) */
	COPY_NEW_FE					/* to/from frontend (3.0 protocol) */
} CopyDest;

/*
 *	Represents the end-of-line terminator type of the input
 */
typedef enum EolType
{
	EOL_UNKNOWN,
	EOL_NL,
	EOL_CR,
	EOL_CRNL
} EolType;

/*
 * This struct contains all the state variables used throughout a COPY
 * operation. For simplicity, we use the same struct for all variants of COPY,
 * even though some fields are used in only some cases.
 *
 * Multi-byte encodings: all supported client-side encodings encode multi-byte
 * characters by having the first byte's high bit set. Subsequent bytes of the
 * character can have the high bit not set. When scanning data in such an
 * encoding to look for a match to a single-byte (ie ASCII) character, we must
 * use the full pg_encoding_mblen() machinery to skip over multibyte
 * characters, else we might find a false match to a trailing byte. In
 * supported server encodings, there is no possibility of a false match, and
 * it's faster to make useless comparisons to trailing bytes than it is to
 * invoke pg_encoding_mblen() to skip over them. encoding_embeds_ascii is TRUE
 * when we have to do it the hard way.
 */
typedef struct CopyStateData
{
	/* low-level state data */
	CopyDest	copy_dest;		/* type of copy source/destination */
	FILE	   *copy_file;		/* used if copy_dest == COPY_FILE */
	StringInfo	fe_msgbuf;		/* used for all dests during COPY TO, only for
								 * dest == COPY_NEW_FE in COPY FROM */
	bool		fe_copy;		/* true for all FE copy dests */
	bool		fe_eof;			/* true if detected end of copy data */
	EolType		eol_type;		/* EOL type of input */
	int			client_encoding;	/* remote side's character encoding */
	bool		need_transcoding;		/* client encoding diff from server? */
	bool		encoding_embeds_ascii;	/* ASCII can be non-first byte? */
	uint64		processed;		/* # of tuples processed */
	size_t		bytesread;
	bool		no_more_data;

	/* parameters from the COPY command */
	Relation	rel;			/* relation to copy to or from */
	QueryDesc  *queryDesc;		/* executable query to copy from */
	List	   *attnumlist;		/* integer list of attnums to copy */
	char	   *filename;		/* filename, or NULL for STDIN/STDOUT */
	bool		binary;			/* binary format? */
	bool		oids;			/* include OIDs? */
	bool		csv_mode;		/* Comma Separated Value format? */
	bool		header_line;	/* CSV header line? */
	char	   *null_print;		/* NULL marker string (server encoding!) */
	int			null_print_len; /* length of same */
	char	   *null_print_client;		/* same converted to client encoding */
	char	   *delim;			/* column delimiter (must be 1 byte) */
	char	   *quote;			/* CSV quote char (must be 1 byte) */
	char	   *escape;			/* CSV escape char (must be 1 byte) */
	bool	   *force_quote_flags;		/* per-column CSV FQ flags */
	bool	   *force_notnull_flags;	/* per-column CSV FNN flags */

	/* these are just for error messages, see copy_in_error_callback */
	const char *cur_relname;	/* table name for error messages */
	int			cur_lineno;		/* line number for error messages */
	const char *cur_attname;	/* current att for error messages */
	//const char *cur_attval;		 /* current att value for error messages */

	/*
	 * Working state for COPY TO
	 */
	FmgrInfo   *out_functions;	/* lookup info for output functions */
	MemoryContext rowcontext;	/* per-row evaluation context */

	/*
	 * These variables are used to reduce overhead in textual COPY FROM.
	 *
	 * attribute_buf holds the separated, de-escaped text for each field of
	 * the current line.  The CopyReadAttributes functions return arrays of
	 * pointers into this buffer.  We avoid palloc/pfree overhead by re-using
	 * the buffer on each cycle.
	 */
	StringInfoData attribute_buf;

	/*
	 * Similarly, line_buf holds the whole input line being processed. The
	 * input cycle is first to read the whole line into line_buf, convert it
	 * to server encoding there, and then extract the individual attribute
	 * fields into attribute_buf.  line_buf is preserved unmodified so that we
	 * can display it in error messages if appropriate.
	 */
	StringInfoData line_buf;

	int		   *attr_offsets;

	bool		line_buf_converted;		/* converted to server encoding? */

	/*
	 * Finally, raw_buf holds raw data read from the data source (file or
																  * client connection). CopyReadLine parses this data sufficiently to
	 * locate line boundaries, then transfers the data to line_buf and
	 * converts it.  Note: we guarantee that there is a \0 at
	 * raw_buf[raw_buf_len].
	 */

#define RAW_BUF_SIZE 65536

	/* NOTE: raw_buf in Greenplum Database is used with different logic than postgres COPY */
	char		raw_buf[RAW_BUF_SIZE + 1];		/* extra byte for '\0' */
	int			raw_buf_index;	/* next byte to process */
	bool		raw_buf_done;	/* finished processing the current buffer */

	/* Greenplum Database specific variables */
	bool		is_copy_in;		/* copy in or out? */
	char		eol_ch[2];		/* The byte values of the 1 or 2 eol bytes */
	bool		escape_off;		/* treat backslashes as non-special? */
	int			last_hash_field;
	bool		line_done;		/* finished processing the whole line or
								 * stopped in the middle */
	bool		end_marker;
	char	   *begloc;
	char	   *endloc;
	bool		error_on_executor;		/* true if errors arrived from the
										 * executors COPY (QEs) */
	StringInfoData executor_err_context;		/* error context text from QE */


	/* for CSV format */
	bool		in_quote;
	bool		last_was_esc;

	/* for TEXT format */
	bool		esc_in_prevbuf; /* escape was last character of the data input
								 * buffer */
	bool		cr_in_prevbuf;	/* CR was last character of the data input
								 * buffer */


	struct HeapInsertDesc *heapInsertDesc;

	/* end Greenplum Database specific variables */

} CopyStateData;

typedef CopyStateData *CopyState;


#define ISOCTAL(c) (((c) >= '0') && ((c) <= '7'))
#define OCTVALUE(c) ((c) - '0')

extern uint64 DoCopy(const CopyStmt *stmt);

extern DestReceiver *CreateCopyDestReceiver(void);

extern List *CopyGetAttnums(TupleDesc tupDesc, Relation rel, List *attnamelist);
extern bool CopyReadLineText(CopyState cstate, size_t bytesread);
extern bool CopyReadLineCSV(CopyState cstate, size_t bytesread);
extern void CopyReadAttributesText(CopyState cstate, char *nulls,
			 int *attr_offsets, int num_phys_attrs, Form_pg_attribute *attr);
extern void CopyReadAttributesCSV(CopyState cstate, char *nulls, int *attr_offsets,
					  int num_phys_attrs, Form_pg_attribute *attr);
extern void limit_printout_length(StringInfo buf);



#endif   /* COPY_H */
