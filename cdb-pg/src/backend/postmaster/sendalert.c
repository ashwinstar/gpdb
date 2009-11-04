/*-------------------------------------------------------------------------
 *
 * alert.c
 *
 * Send alerts via SMTP (email) or SNMP INFORM messsages.
 *
 * Copyright (c) 2009, Greenplum
 *
 *
 *-------------------------------------------------------------------------
 */
#define _XOPEN_SOURCE 500
#include "postgres.h"
#include "pg_config.h"  /* Adding this helps eclipse see that USE_EMAIL and USE_SNMP are set */

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdio.h>

#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>

#include "lib/stringinfo.h"

#include "pgtime.h"

#include "postmaster/syslogger.h"
#include "postmaster/sendalert.h"
#include "utils/guc.h"
#include "utils/elog.h"
#include "utils/builtins.h"
extern int	PostPortNumber;
extern void pg_itoa(int16 i, char *a);

#if USE_EMAIL
#if USE_SSL
#include <openssl/ssl.h>
#endif
/* SASL (RFC 2222) client library API */
#include <auth-client.h>
#include <libesmtp.h>
#endif

#if USE_SNMP
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#endif

#if USE_EMAIL

static char * get_str_from_chunk(CSVChunkStr *chunkstr,
		const PipeProtoChunk *saved_chunks);
static const char * messagebody_cb(void **buf, int *len, void *arg);
static void print_recipient_status(smtp_recipient_t recipient,
		const char *mailbox, void *arg  __attribute__((unused)));
static int authinteract(auth_client_request_t request, char **result,
		int fields, void *arg);
static int tlsinteract(char *buf, int buflen, int rwflag  __attribute__((unused)), void *arg  __attribute__((unused)));
static int handle_invalid_peer_certificate(long vfy_result);
static void event_cb(smtp_session_t session, int event_no, void *arg, ...);
#endif

static FILE * mylog = NULL;

int send_alert_from_chunks(const PipeProtoChunk *chunk,
		const PipeProtoChunk * saved_chunks_in)
{

	int ret = -1;
	GpErrorData errorData;

	CSVChunkStr chunkstr =
	{ chunk, chunk->data + sizeof(GpErrorDataFixFields) };

	if (mylog == NULL)
			mylog = fopen("/tmp/mylog.log", "a");

	memset(&errorData, 0, sizeof(errorData));

	memcpy(&errorData.fix_fields, chunk->data, sizeof(errorData.fix_fields));

	if (chunk == NULL)
		return -1;
	if (chunk->hdr.len == 0)
		return -1;
	if (chunk->hdr.zero != 0)
		return -1;

	if (chunk->hdr.log_format != 'c')
		elog(ERROR,"send_alert_from_chunks only works when CSV logging is enabled");

	errorData.username = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.databasename = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.remote_host = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.remote_port = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.error_severity = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.sql_state = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.error_message = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.error_detail = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.error_hint = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.internal_query = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.error_context = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.debug_query_string = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.error_func_name = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.error_filename = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.stacktrace = get_str_from_chunk(&chunkstr,saved_chunks_in);


	PG_TRY();
	{
	ret = send_alert(&errorData);
	}
	PG_CATCH();
	{
		elog(LOG,"send_alert failed.  Not sending the alert");
		free(errorData.stacktrace ); errorData.stacktrace = NULL;
		free((char *)errorData.error_filename ); errorData.error_filename = NULL;
		free((char *)errorData.error_func_name ); errorData.error_func_name = NULL;
		free(errorData.debug_query_string ); errorData.debug_query_string = NULL;
		free(errorData.error_context); errorData.error_context = NULL;
		free(errorData.internal_query ); errorData.internal_query = NULL;
		free(errorData.error_hint ); errorData.error_hint = NULL;
		free(errorData.error_detail ); errorData.error_detail = NULL;
		free(errorData.error_message ); errorData.error_message = NULL;
		free(errorData.sql_state ); errorData.sql_state = NULL;
		free((char *)errorData.error_severity ); errorData.error_severity = NULL;
		free(errorData.remote_port ); errorData.remote_port = NULL;
		free(errorData.remote_host ); errorData.remote_host = NULL;
		free(errorData.databasename ); errorData.databasename = NULL;
		free(errorData.username ); errorData.username = NULL;
		/* Carry on with error handling. */
		PG_RE_THROW();
	}
	PG_END_TRY();

	// Don't forget to free them!  Best in reverse order of the mallocs.

	free(errorData.stacktrace ); errorData.stacktrace = NULL;
	free((char *)errorData.error_filename ); errorData.error_filename = NULL;
	free((char *)errorData.error_func_name ); errorData.error_func_name = NULL;
	free(errorData.debug_query_string ); errorData.debug_query_string = NULL;
	free(errorData.error_context); errorData.error_context = NULL;
	free(errorData.internal_query ); errorData.internal_query = NULL;
	free(errorData.error_hint ); errorData.error_hint = NULL;
	free(errorData.error_detail ); errorData.error_detail = NULL;
	free(errorData.error_message ); errorData.error_message = NULL;
	free(errorData.sql_state ); errorData.sql_state = NULL;
	free((char *)errorData.error_severity ); errorData.error_severity = NULL;
	free(errorData.remote_port ); errorData.remote_port = NULL;
	free(errorData.remote_host ); errorData.remote_host = NULL;
	free(errorData.databasename ); errorData.databasename = NULL;
	free(errorData.username ); errorData.username = NULL;

	return ret;
}


int send_alert(const GpErrorData * errorData)
{
#if USE_EMAIL
	smtp_session_t session;
	smtp_message_t message;
	smtp_recipient_t recipient;
	auth_context_t authctx;
	const smtp_status_t *status;

	char subject[128];
	enum notify_flags notify = Notify_FAILURE;
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	int			success = 0;

#endif
	if (mylog == NULL)
		mylog = fopen("/tmp/mylog.log", "a");

	fprintf(mylog,"send_alert: %s: %s\n%s\n",errorData->error_severity, errorData->error_message, errorData->error_detail);
	fflush(mylog);

	elog(DEBUG2,"send_alert: %s: %s",errorData->error_severity, errorData->error_message);

	/*
	 * SIGPIPE must be ignored, or we will have problems.
	 *
	 * but this is already set in syslogger.c, so we are OK
	 * //pqsignal(SIGPIPE, SIG_IGN);  // already set in syslogger.c
	 *
	 */

#if USE_EMAIL

	//host = "smtpx18.msoutlookonline.net:587";
	//"greenplum.com.s5a2.psmtp.com:25";


	if (strlen(email_from) == 0 || strlen(email_to) == 0)
	{
		elog(LOG,"e-mail alerts are not properly configured");
		return -1;
	}

	/* Set up a subject line for the e-mail.  Let's limit it to 127 bytes just to be safe. */
	if (strcmp(errorData->error_severity,"LOG") == 0)
		snprintf(subject, 127, "%s",errorData->error_message);
	else
		snprintf(subject, 127, "%s: %s",errorData->error_severity, errorData->error_message);
	subject[127] = '\0'; /* Guarantee subject is zero terminated */

	// Ok, send the alert here.

	auth_client_init(); // Call this only once!
	session = smtp_create_session();
	message = smtp_add_message(session);

#if USE_SSL
	smtp_starttls_enable(session, Starttls_ENABLED);
#endif

	/*
	 * Set the host running the SMTP server.  LibESMTP has a default port
	 * number of 587, however this is not widely deployed so the port
	 * is specified as 25 along with the default MTA host.
	 */
	smtp_set_server(session, email_smtp_server != NULL && strlen(email_smtp_server) > 0 ? email_smtp_server : "localhost:25");

	smtp_set_eventcb(session, event_cb, NULL);

	/*
	 * Do what's needed at application level to use authentication.
	 */
	authctx = auth_create_context();
	auth_set_mechanism_flags(authctx, AUTH_PLUGIN_PLAIN|AUTH_PLUGIN_ANONYMOUS, 0);
	auth_set_interact_cb(authctx, authinteract, NULL);

	/* I don't know how "external" is supposed to work, or what ID I'm supposed to set here */
	auth_set_external_id(authctx, email_smtp_userid);

	smtp_auth_set_context(session, authctx);

	/*
	 * Use our callback for X.509 certificate passwords.  If STARTTLS is
	 * not in use or disabled in configure, the following is harmless.
	 */
	smtp_starttls_set_password_cb(tlsinteract, NULL);

	/*
	 * Set the reverse path for the mail envelope.  (NULL is ok)
	 */
	smtp_set_reverse_path(message, email_from);

	smtp_set_header(message, "Subject", subject);
	smtp_set_header_option(message, "Subject", Hdr_OVERRIDE, 1);
	smtp_set_header(message, "Message-Id", NULL);

	smtp_set_messagecb(message, messagebody_cb, (void *) errorData);

	/* Need a modifiable copy of To list */
	rawstring = pstrdup(email_to);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		ereport(LOG,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax for \"email_to\"")));
	}

	foreach(l, elemlist)
	{
		char	   *cur_email_addr = (char *) lfirst(l);

		if (cur_email_addr != NULL && strlen(cur_email_addr) > 0)
		{
			/*
			 * RFC 2822 doesn't require recipient headers but a To: header would
			 * be nice to have if not present.
			 */
			smtp_set_header(message, "To", cur_email_addr, cur_email_addr);

			/* Recipient must be in RFC2821 format */
			recipient = smtp_add_recipient(message, cur_email_addr);
			success = true;
		}

	}

	if (!success && list_length(elemlist))
		ereport(LOG,
				(errmsg("Could not understand e-mail To: list")));

	list_free(elemlist);
	pfree(rawstring);


	smtp_dsn_set_notify(recipient, notify);

	smtp_8bitmime_set_body(message, E8bitmime_8BITMIME);

	/* Initiate a connection to the SMTP server and transfer the message. */
	if (!smtp_start_session(session))
	{
		/*
		 * If we get here, we can't talk to the SMTP server at all
		 */
		char buf[128];

		elog(LOG, "SMTP server problem %s", smtp_strerror(smtp_errno(),
				buf, sizeof buf));
	}
	else
	{
		/*
		 * Report on the success or otherwise of the mail transfer.
		 * This really only tells the "overall" status, so if an individual e-mail fails,
		 * we won't see it here (even if every one of them fails!).
		 */

		/*
		 * status 250 == ok
		 * > 300 is errors
		 */
		status = smtp_message_transfer_status(message);
		elog(status->code < 300 ? DEBUG1 : LOG, "overall transfer status: %d %s\n", status->code,
				(status->text != NULL) ? status->text : "");

		/*
		 * This should tell us how each e-mail recipient fared.
		 */
		smtp_enumerate_recipients(message, print_recipient_status, NULL);
	}

	/* Free resources consumed by the program.
	 */
	smtp_destroy_session(session);
	auth_destroy_context(authctx);

	auth_client_exit();
	fflush(mylog);
#endif

	return 0;
}


static size_t
pg_strnlen(const char *str, size_t maxlen)
{
	const char *p = str;

	while (maxlen-- > 0 && *p)
		p++;
	return p - str;
}

static void move_to_next_chunk(CSVChunkStr * chunkstr,
		const PipeProtoChunk * saved_chunks)
{
	if (chunkstr->chunk != NULL)
		if (chunkstr->p - chunkstr->chunk->data >= chunkstr->chunk->hdr.len)
		{
			/* switch to next chunk */
			if (chunkstr->chunk->hdr.next >= 0)
			{
				chunkstr->chunk = &saved_chunks[chunkstr->chunk->hdr.next];
				chunkstr->p = chunkstr->chunk->data;
			}
			else
			{
				/* no more chunks */
				chunkstr->chunk = NULL;
				chunkstr->p = NULL;
			}
		}
}

static char *
get_str_from_chunk(CSVChunkStr *chunkstr, const PipeProtoChunk *saved_chunks)
{
	int wlen = 0;
	int len = 0;
	char * out = NULL;

	move_to_next_chunk(chunkstr, saved_chunks);

	if (chunkstr->p == NULL)
	{
		return strdup("");
	}

	len = chunkstr->chunk->hdr.len - (chunkstr->p - chunkstr->chunk->data);

	/* Check if the string is an empty string */
	if (len > 0 && chunkstr->p[0] == '\0')
	{
		chunkstr->p++;
		move_to_next_chunk(chunkstr, saved_chunks);

		return strdup("");
	}

	if (len == 0 && chunkstr->chunk->hdr.next >= 0)
	{
		const PipeProtoChunk *next_chunk =
				&saved_chunks[chunkstr->chunk->hdr.next];
		if (next_chunk->hdr.len > 0 && next_chunk->data[0] == '\0')
		{
			chunkstr->p++;
			if (mylog)
				fprintf(mylog, "chunkstr->p has empty string2\n");
			move_to_next_chunk(chunkstr, saved_chunks);
			return strdup("");
		}
	}

	wlen = pg_strnlen(chunkstr->p, len);

	if (wlen < len)
	{
		// String all contained in this chunk
		out = malloc(wlen + 1);
		memcpy(out, chunkstr->p, wlen + 1); // include the null byte
		chunkstr->p += wlen + 1; // skip to start of next string.
		return out;
	}

	out = malloc(wlen + 1);
	memcpy(out, chunkstr->p, wlen);
	out[wlen] = '\0';
	chunkstr->p += wlen;
	if (out != NULL && mylog != NULL)
	fprintf(mylog, "partial string: %s\n", out);
	fflush(mylog);

	while (chunkstr->p)
	{
		move_to_next_chunk(chunkstr, saved_chunks);
		if (chunkstr->p == NULL)
			break;
		len = chunkstr->chunk->hdr.len - (chunkstr->p - chunkstr->chunk->data);

		wlen = pg_strnlen(chunkstr->p, len);

		/* Write OK, don't forget to account for the trailing 0 */
		if (wlen < len)
		{
			// Remainder of String all contained in this chunk
			out = realloc(out, strlen(out) + wlen + 1);
			strncat(out, chunkstr->p, wlen + 1); // include the null byte

			fprintf(mylog, "new fragment string: %s\n", chunkstr->p);
			fprintf(mylog, "new  string: %s\n", out);
			fflush(mylog);
			chunkstr->p += wlen + 1; // skip to start of next string.
			fprintf(mylog, "next string: %s\n", chunkstr->p);
			fflush(mylog);
			return out;
		}
		else
		{
			int newlen = strlen(out) + wlen;
			out = realloc(out, newlen + 1);
			strncat(out, chunkstr->p, wlen);
			out[newlen] = '\0';

			fprintf(mylog, "partial string: %s\n", out);
			fflush(mylog);

			chunkstr->p += wlen;
		}
	}

	return out;
}

#if USE_EMAIL

/*
 *  The message is read a line at a time and the newlines converted
 *  to \r\n.  Unfortunately, RFC 822 states that bare \n and \r are
 *  acceptable in messages and that individually they do not constitute a
 *  line termination.  This requirement cannot be reconciled with storing
 *  messages with Unix line terminations.  RFC 2822 rescues this situation
 *  slightly by prohibiting lone \r and \n in messages.
 *
 */
static void add_to_message(char * message, const char * newstr_in)
{
	const char * newstr = newstr_in;
	char * p;
	if (newstr == NULL)
		return;

	/* Drop any leading \n characters:  Not sure what to do with them */
	while (*newstr == '\n')
		newstr++;

	/* Scan for \n, and convert to \r\n */
	while ((p = strchr(newstr,'\n')) != NULL)
	{
		/* Don't exceed 900 chars added to this line, so total line < 1000 */
		if (p - newstr >= 900)
		{
			strncat(message, newstr, 898);
			strcat(message, "\r\n\t");
			newstr += 898;
		}
		else if (p - newstr >=2 && *(p-1) != '\r')
		{
			strncat(message, newstr, p - newstr);
			strcat(message, "\r\n\t");
			newstr = p+1;
		}
		else
		{
			strncat(message, newstr, p - newstr + 1);
			newstr = p+1;
		}
	}
	strcat(message, newstr);

}
static const char *
messagebody_cb(void **buf, int *len, void *arg)
{
	const GpErrorData *errorData = (GpErrorData *) arg;
	char * message = NULL;
	static int alreadyreturned = 0;

	if (*buf == NULL)
	{
		alreadyreturned = 0;
		if (len == NULL)
			return NULL;

		*buf = malloc(8192);
		message = ((char *)(*buf));
		message[0] = '\0';
	}

	if (len == NULL)
	{
		alreadyreturned = 0;
		message = ((char *)(*buf));
		message[0] = '\0';
	    return NULL;
	}

	message = ((char *)(*buf));

	if (alreadyreturned == 0)
	{
		char  lineno[10];

		pg_itoa(errorData->fix_fields.error_fileline,lineno);

		/* Perhaps better to use text/html ? */
		strcpy(message,
			//"Return-Path: <cmcdevitt@greenplum.com>\r\n"
			//"Subject: LibESMTP test mail\r\n"
			"MIME-Version: 1.0\r\n"
			"Content-Type: text/plain;\r\n"
			"  charset=utf-8\r\n"
			"Content-Transfer-Encoding: 8bit\r\n\r\n");

		/* Lines must be < 1000 bytes long for 7bit or 8bit transfer-encoding */
		/* Perhaps use base64 encoding instead?  Or just wrap them? */

		/* How to identify which system is sending the alert?  Perhaps our hostname and port is good enough? */
		strcat(message,"Alert from GPDB system ");
		{
			char myhostname[255];
			char portnum[7];
			myhostname[0] = '\0';


			if (gethostname(myhostname, sizeof(myhostname)) == 0)
			{
				strcat(message,myhostname);
				strcat(message," on port ");
				pg_itoa(PostPortNumber,portnum);
				strcat(message,portnum);
			}
		}
		strcat(message,":\r\n\r\n");
		if (errorData->username != NULL &&  errorData->databasename != NULL &&
				strlen(errorData->username)>0 && strlen(errorData->databasename)>0)
		{
			strcat(message, errorData->username);
			if (errorData->remote_host != NULL && strlen(errorData->remote_host) > 0)
			{
				if (strcmp(errorData->remote_host,"[local]")==0)
					strcat(message, " connected locally on master node");
				else
				{
					strcat(message," from host ");
					strcat(message,errorData->remote_host);
				}
			}
			strcat(message, " connected to database ");
			strcat(message, errorData->databasename);
			strcat(message, "\r\n");

		}
		if (errorData->fix_fields.gp_segment_id != -1)
		{
			strcat(message,"Error occurred on segment ");
		}
		else
			strcat(message,"Error occurred on master segment");
		strcat(message, "\r\n");
		strcat(message, "\r\n");

		strcat(message, errorData->error_severity);
		strcat(message, ": ");
		if (errorData->sql_state != NULL && pg_strnlen(errorData->sql_state,5)>4 && strncmp(errorData->sql_state,"00000",5)!=0)
		{
			strcat(message, "(");
			strncat(message, errorData->sql_state, 5);
			strcat(message, ") ");
		}
		add_to_message(message, errorData->error_message);
		strcat(message, "\r\n");
		strcat(message, "\r\n");

		if (errorData->error_detail != NULL &&strlen(errorData->error_detail) > 0)
		{
			strcat(message, _("DETAIL:  "));
			add_to_message(message, errorData->error_detail);
			strcat(message, "\r\n");
		}
		if (errorData->error_hint != NULL &&strlen(errorData->error_hint) > 0)
		{
			strcat(message, _("HINT:  "));
			add_to_message(message, errorData->error_hint);
			strcat(message, "\r\n");
		}
		if (errorData->internal_query != NULL &&strlen(errorData->internal_query) > 0)
		{
			strcat(message, _("QUERY:  "));
			add_to_message(message, errorData->internal_query);
			strcat(message, "\r\n");
		}
		if (errorData->error_context != NULL && strlen(errorData->error_context) > 0)
		{
			strcat(message, _("CONTEXT:  "));
			add_to_message(message, errorData->error_context);
			strcat(message, "\r\n");
		}
		if (errorData->fix_fields.omit_location != 't')
		{
			if (errorData->error_filename != NULL && strlen(errorData->error_filename) > 0)
			{
				strcat(message,  _("LOCATION:  "));

				if (errorData->error_func_name && strlen(errorData->error_func_name) > 0)
				{
					strcat(message, errorData->error_func_name);
					strcat(message, ", ");
				}

				strcat(message, errorData->error_filename);
				strcat(message, ":");
				strcat(message, lineno);
				strcat(message, "\r\n");
			}
			if (errorData->stacktrace != NULL && strlen(errorData->stacktrace) > 0)
			{
				strcat(message, "STACK TRACE:\r\n\t");
				add_to_message(message, errorData->stacktrace);
				strcat(message, "\r\n");
			}
		}
		if (errorData->debug_query_string != NULL &&strlen(errorData->debug_query_string) > 0)
		{
			strcat(message, _("STATEMENT:  "));
			add_to_message(message, errorData->debug_query_string);
			strcat(message, "\r\n");
		}
	}

	if (alreadyreturned < strlen(message))
	{
		char * p;
		message += alreadyreturned;
		if ((p = strchr(message,'\n')) != NULL)
		{
			*len = p - message + 1;
			alreadyreturned += *len;
		}
		else if (strlen(message)>0)
		{
			*len = strlen(message);
			alreadyreturned += *len;
			fprintf(mylog, "message, without rn?: %.*s",*len, message);
			fflush(mylog);
		}
		else
		{
			fprintf(mylog, "message finished, returning zero\n");
			fflush(mylog);
			*len = 0;
			message = NULL;
		}
	}
	else
	{
		*len = 0;
		return NULL;
	}

	return message;
}

/* Callback to print the recipient status */
static void print_recipient_status(smtp_recipient_t recipient,
		const char *mailbox, void *arg  __attribute__((unused)))
{
	const smtp_status_t *status;

	status = smtp_recipient_status(recipient);
	if (status->code != 0 && status->code != 250)
	{
		elog(LOG, "recipient status: %s: %d %s", mailbox, status->code, status->text);
	}
}

static int authinteract(auth_client_request_t request, char **result,
		int fields, void *arg)
{
	int i;

	/* SASL auth requests */
	for (i = 0; i < fields; i++)
	{
		if (request[i].flags & AUTH_PASS)
		{
			elog(DEBUG1, "authinteract asking for password: %s  %s%s\n",request[i].name, request[i].prompt,(request[i].flags & AUTH_CLEARTEXT) ? " (not encrypted)"
					: "");
			result[i] = email_smtp_password;
		}
		else if (request[i].flags & AUTH_USER)
		{
			elog(DEBUG1, "authinteract asking for username: %s  %s%s\n",request[i].name, request[i].prompt,(request[i].flags & AUTH_CLEARTEXT) ? " (not encrypted)"
					: "");
			result[i] = email_smtp_userid;

		}
		else if (request[i].flags & AUTH_REALM)
		{
			elog(LOG, "authinteract asking for realm: %s  %s%s\n",request[i].name, request[i].prompt,(request[i].flags & AUTH_CLEARTEXT) ? " (not encrypted)"
					: "");
			result[i] = "greenplum.com";

		}
		else if (result[i] == NULL)
		{
			return 0;
		}
	}

	return 1;
}

static int tlsinteract(char *buf, int buflen, int rwflag  __attribute__((unused)), void *arg  __attribute__((unused)))
{

	elog(LOG, "tlsinteract asking for tls password\n");

	strcpy(buf, email_smtp_password);
	return strlen(buf);
}

static int handle_invalid_peer_certificate(long vfy_result)
{
	const char *k = "rare error";
	switch (vfy_result)
	{
	case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
		k = "X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT";
		break;
	case X509_V_ERR_UNABLE_TO_GET_CRL:
		k = "X509_V_ERR_UNABLE_TO_GET_CRL";
		break;
	case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE:
		k = "X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE";
		break;
	case X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE:
		k = "X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE";
		break;
	case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY:
		k = "X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY";
		break;
	case X509_V_ERR_CERT_SIGNATURE_FAILURE:
		k = "X509_V_ERR_CERT_SIGNATURE_FAILURE";
		break;
	case X509_V_ERR_CRL_SIGNATURE_FAILURE:
		k = "X509_V_ERR_CRL_SIGNATURE_FAILURE";
		break;
	case X509_V_ERR_CERT_NOT_YET_VALID:
		k = "X509_V_ERR_CERT_NOT_YET_VALID";
		break;
	case X509_V_ERR_CERT_HAS_EXPIRED:
		k = "X509_V_ERR_CERT_HAS_EXPIRED";
		break;
	case X509_V_ERR_CRL_NOT_YET_VALID:
		k = "X509_V_ERR_CRL_NOT_YET_VALID";
		break;
	case X509_V_ERR_CRL_HAS_EXPIRED:
		k = "X509_V_ERR_CRL_HAS_EXPIRED";
		break;
	case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
		k = "X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD";
		break;
	case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
		k = "X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD";
		break;
	case X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD:
		k = "X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD";
		break;
	case X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD:
		k = "X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD";
		break;
	case X509_V_ERR_OUT_OF_MEM:
		k = "X509_V_ERR_OUT_OF_MEM";
		break;
	case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
		k = "X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT";
		break;
	case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
		k = "X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN";
		break;
	case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
		k = "X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY";
		break;
	case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
		k = "X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE";
		break;
	case X509_V_ERR_CERT_CHAIN_TOO_LONG:
		k = "X509_V_ERR_CERT_CHAIN_TOO_LONG";
		break;
	case X509_V_ERR_CERT_REVOKED:
		k = "X509_V_ERR_CERT_REVOKED";
		break;
	case X509_V_ERR_INVALID_CA:
		k = "X509_V_ERR_INVALID_CA";
		break;
	case X509_V_ERR_PATH_LENGTH_EXCEEDED:
		k = "X509_V_ERR_PATH_LENGTH_EXCEEDED";
		break;
	case X509_V_ERR_INVALID_PURPOSE:
		k = "X509_V_ERR_INVALID_PURPOSE";
		break;
	case X509_V_ERR_CERT_UNTRUSTED:
		k = "X509_V_ERR_CERT_UNTRUSTED";
		break;
	case X509_V_ERR_CERT_REJECTED:
		k = "X509_V_ERR_CERT_REJECTED";
		break;
	}

	/*
	 * These two errors come from using self-signed certificates.  We don't care about that.
	 */
	if (vfy_result != X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY &&
		vfy_result != X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN)
		fprintf(mylog, "SMTP_EV_INVALID_PEER_CERTIFICATE: %ld: %s\n", vfy_result, k);

	return 1; /* Accept the problem and try to continue  */
}

/*
 * We currently just return 1 to keep libesmtp happy
 * Will turn this into a proper event handler some day
 */
static void event_cb(smtp_session_t session, int event_no, void *arg, ...)
{
	va_list alist;
	int *ok;

	va_start(alist, arg);

	switch (event_no)
	{

	case SMTP_EV_WEAK_CIPHER:
		{
			long bits;

			bits = va_arg(alist, long);
			ok = va_arg(alist, int*);
			fprintf(mylog,"WEAK_CIPHER bits=%ld\n",bits);
			*ok = 1;
		}
		break;

	case SMTP_EV_INVALID_PEER_CERTIFICATE:
		{
			long vfy_result;
			vfy_result = va_arg(alist, long);
			ok = va_arg(alist, int*);
			*ok = handle_invalid_peer_certificate(vfy_result);
		}
		break;

	case SMTP_EV_NO_PEER_CERTIFICATE:
		{
			ok = va_arg(alist, int*);
			fprintf(mylog,"NO PEER CERT\n");
			*ok = 1;
		}
		break;
	case SMTP_EV_WRONG_PEER_CERTIFICATE:
		{
			ok = va_arg(alist, int*);
			elog(DEBUG2,"SMTP_EV_WRONG_PEER_CERTIFICATE");
			*ok = 1;
		}
		break;
	case SMTP_EV_NO_CLIENT_CERTIFICATE:
		{
			ok = va_arg(alist, int*);
			fprintf(mylog,"NO CLIENT CERT\n");
			*ok = 1;
		}
		break;

	case SMTP_EV_STARTTLS_OK:
		elog(DEBUG2,"SMTP_EV_STARTTLS_OK");
		break;
	case SMTP_EV_CONNECT:
		elog(DEBUG1,"SMTP_EV_CONNECT");
		break;
	case SMTP_EV_MAILSTATUS:
	{
		const smtp_status_t *status = NULL;

		smtp_message_t msg;
		const char * rev_path;
		rev_path = va_arg(alist, const char*);
		msg = va_arg(alist, smtp_message_t);

		if (msg != NULL)
		status = smtp_reverse_path_status (msg);

		if (status && status->code != 250 && status->text && rev_path)
		fprintf(mylog,"EV_MAILSTATUS %d:  %s:  %s\n", status->code, rev_path,  status->text);

		if (status && status->code != 0 && status->code != 250 && status->text != NULL)
			elog(LOG,"EV_MAILSTATUS: SMTP server reports a problem %d:  %s:  %s", status->code, rev_path,  status->text);
	}
	break;
	case SMTP_EV_RCPTSTATUS:
#if 0
	{
			const smtp_status_t *status = NULL;

			smtp_recipient_t msg;
				msg = va_arg(alist, smtp_recipient_t);
				if (msg != NULL)
				status = smtp_recipient_status (msg);



			if (status && status->code !=250 && status->text)
				fprintf(mylog,"SMTP_EV_RCPTSTATUS status = %d %s\n",status->code, status->text);

			if (status && status->code != 0 && status->code != 250 && status->text != NULL)
				elog(LOG,"SMTP server reports a RCPTSTATUS problem %d: %s", status->code, status->text);
		}
#endif
	fprintf(mylog,"SMTP_EV_RCPTSTATUS\n");
		break;
	case SMTP_EV_MESSAGEDATA:
		/*{
			smtp_message_t * msg;
			int mlen;
			msg = va_arg(alist, smtp_message_t);
			mlen = va_arg(alist, int);
			fprintf(mylog, "EV_MESSAGEDATA len = %d\n", mlen);
		}*/
		break;
	case SMTP_EV_MESSAGESENT:
	{
		const smtp_status_t *status = NULL;

			smtp_message_t msg;
			msg = va_arg(alist, smtp_message_t);
			if (msg != NULL)
			status = smtp_message_transfer_status (msg);



		if (status && status->code !=250 && status->text)
			fprintf(mylog,"SMTP_EV_MESSAGESENT status = %d %s\n",status->code, status->text);

		if (status && status->code != 0 && status->code != 250 && status->text != NULL)
			elog(LOG,"SMTP server reports a messagesent problem %d: %s", status->code, status->text);
	}
	break;
	case SMTP_EV_DISCONNECT:
		elog(DEBUG2, "EV_DISCONNECT\n");
		break;
	default:
		break;

	}

	va_end(alist);
}
#endif

