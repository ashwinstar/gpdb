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
#include "postgres_fe.h"

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdio.h>

#include "lib/stringinfo.h"
#include "libpq/pqsignal.h"

#include "pgtime.h"

#include "postmaster/syslogger.h"
#include "postmaster/sendalert.h"
//#include "miscadmin.h"
//#include "utils/guc.h"
#include "utils/elog.h"

#if USE_EMAIL
#if USE_SSL
#include <openssl/ssl.h>
#endif
#include <auth-client.h>
#include <libesmtp.h>
#endif

#if 1
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#endif


#if USE_EMAIL

static char * get_str_from_chunk(CSVChunkStr *chunkstr,const PipeProtoChunk *saved_chunks);
static const char * messagebody_cb(void **buf, int *len, void *arg);
static int authinteract(auth_client_request_t request, char **result,
		int fields, void *arg);
static void event_cb(smtp_session_t session, int event_no, void *arg, ...);
#endif
static FILE * mylog = NULL;

int
send_alert_from_chunks(const PipeProtoChunk *chunk, const PipeProtoChunk * saved_chunks_in)
{

	int ret = -1;
	GpErrorData errorData;

	//GpErrorDataFixFields *pfixed = (GpErrorDataFixFields *) (chunk->data);
	CSVChunkStr chunkstr = { chunk, chunk->data + sizeof(GpErrorDataFixFields) };


	if (mylog == NULL)
			mylog = fopen("/tmp/mylog.log","a");


	memset(&errorData,0,sizeof(errorData));

	memcpy(&errorData.fix_fields, chunk->data, sizeof(errorData.fix_fields));

	if (chunk==NULL)
		return -1;
	if (chunk->hdr.len == 0)
		return -1;
	if (chunk->hdr.zero != 0)
		return -1;

	//pqsignal(SIGSEGV, SIG_DFL);

	fprintf(mylog," In send_alert_from_chunks\n");
	fflush(mylog);

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
	errorData.debug_query_string = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.error_func_name = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.error_filename = get_str_from_chunk(&chunkstr,saved_chunks_in);
	errorData.stacktrace = get_str_from_chunk(&chunkstr,saved_chunks_in);

	fprintf(mylog,"%s: %s\n%s\n",errorData.error_severity, errorData.error_message, errorData.error_detail);
	fflush(mylog);

	ret = send_alert(&errorData);
	// Don't forget to free them!

	free(errorData.username ); errorData.username = NULL;
	free(errorData.databasename ); errorData.databasename = NULL;
	free(errorData.remote_host ); errorData.remote_host = NULL;
	free(errorData.remote_port ); errorData.remote_port = NULL;
	free((char *)errorData.error_severity ); errorData.error_severity = NULL;
	free(errorData.sql_state ); errorData.sql_state = NULL;
	free(errorData.error_message ); errorData.error_message = NULL;
	free(errorData.error_detail ); errorData.error_detail = NULL;
	free(errorData.error_hint ); errorData.error_hint = NULL;
	free(errorData.internal_query ); errorData.internal_query = NULL;
	free(errorData.debug_query_string ); errorData.debug_query_string = NULL;
	free((char *)errorData.error_func_name ); errorData.error_func_name = NULL;
	free((char *)errorData.error_filename ); errorData.error_filename = NULL;
	free(errorData.stacktrace ); errorData.stacktrace = NULL;

	fflush(mylog);
	return ret;

}
int
send_alert(const GpErrorData * errorData)
{
#if USE_EMAIL
	smtp_session_t session;
	smtp_message_t message;
	smtp_recipient_t recipient;
	auth_context_t authctx;
	const smtp_status_t *status;
	//struct sigaction sa;

	char *host = NULL;
	char *from = NULL;
	char *subject = NULL;
	//enum notify_flags notify = Notify_NOTSET;

	fprintf(mylog," In send_alert\n");
	fflush(mylog);

	//pqsignal(SIGPIPE, SIG_IGN);  // already set in syslogger.c
	//
	host = "scriptmail.intermedia.net";
	from = "cmcdevitt@greenplum.com";
	subject = "test alert";
	// Ok, send the alert here.

	auth_client_init(); // Call this only once!
	session = smtp_create_session();
	message = smtp_add_message(session);

	//smtp_starttls_enable(session, Starttls_ENABLED);


	smtp_set_server(session, host ? host : "localhost:25");
	fprintf(mylog," did set_server\n");
	fflush(mylog);

	/* Do what's needed at application level to use authentication.
	 */
	authctx = auth_create_context();
	auth_set_mechanism_flags(authctx, AUTH_PLUGIN_PLAIN, 0);
	auth_set_interact_cb(authctx, authinteract, NULL);

	smtp_set_eventcb(session, event_cb, NULL);

	smtp_auth_set_context(session, authctx);

	/* Set the reverse path for the mail envelope.  (NULL is ok)
	 */
	smtp_set_reverse_path(message, from);

	/* RFC 2822 doesn't require recipient headers but a To: header would
	 be nice to have if not present. */
	smtp_set_header(message, "To", NULL, NULL);

	smtp_set_header(message, "Subject", subject);
	smtp_set_header_option(message, "Subject", Hdr_OVERRIDE, 1);
	smtp_set_header(message, "Message-Id", NULL);

	smtp_set_messagecb(message, messagebody_cb, (void *)errorData);

	/* Recipient must be in RFC2821 format */

	recipient = smtp_add_recipient(message, "cmcdevitt@greenplum.com");
	smtp_dsn_set_notify(recipient, Notify_SUCCESS | Notify_FAILURE);
	/* Initiate a connection to the SMTP server and transfer the
	 message. */
	if (!smtp_start_session(session))
	{
		char buf[128];

		fprintf(stderr, "SMTP server problem %s\n", smtp_strerror(smtp_errno(),
				buf, sizeof buf));
	}
	else
	{
		/* Report on the success or otherwise of the mail transfer.
		 */
		status = smtp_message_transfer_status(message);
		printf("%d %s", status->code, (status->text != NULL) ? status->text
				: "\n");
		//smtp_enumerate_recipients(message, print_recipient_status, NULL);
		smtp_destroy_session(session);
		auth_destroy_context(authctx);

		auth_client_exit();

		return (-1);

	}

	/* Free resources consumed by the program.
	 */
	smtp_destroy_session(session);
	auth_destroy_context(authctx);

	auth_client_exit();
#endif
	fprintf(stderr," returning from send_alert\n");
	fflush(mylog);

	return 0;
}


static void move_to_next_chunk(CSVChunkStr * chunkstr,const PipeProtoChunk * saved_chunks)
{
	if (chunkstr->chunk != NULL)
		if(chunkstr->p - chunkstr->chunk->data >= chunkstr->chunk->hdr.len)
		{
			/* switch to next chunk */
			fprintf(mylog,"switch to next chunk\n");
			fflush(mylog);
			if(chunkstr->chunk->hdr.next >= 0)
			{
				fprintf(mylog,"found next chunk\n");
							fflush(mylog);
				chunkstr->chunk = &saved_chunks[chunkstr->chunk->hdr.next];
				chunkstr->p = chunkstr->chunk->data;

			}
			else
			{
				// no more chunks
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

    move_to_next_chunk(chunkstr,saved_chunks);

    if (chunkstr->p == NULL)
    {
		fprintf(mylog,"past end of chunks\n");
		fflush(mylog);
		return strdup("");
	}



	len = chunkstr->chunk->hdr.len - (chunkstr->p - chunkstr->chunk->data);
	if (chunkstr->chunk->hdr.len < (chunkstr->p - chunkstr->chunk->data))
		fprintf(mylog,"hdr.len %d\n",chunkstr->chunk->hdr.len);
	fprintf(mylog,"len of chunk %d\n",len);

	/* Check if the string is an empty string */
	if (len > 0 && chunkstr->p[0] == '\0')
	{
		fprintf(mylog,"chunkstr->p has empty string\n");move_to_next_chunk(chunkstr,saved_chunks);
		chunkstr->p++;
		return strdup("");
	}

	if (len == 0 && chunkstr->chunk->hdr.next >= 0)
	{
		const PipeProtoChunk *next_chunk = &saved_chunks[chunkstr->chunk->hdr.next];
		if (next_chunk->hdr.len > 0 && next_chunk->data[0] == '\0')
		{
			fprintf(mylog,"chunkstr->p has empty string2\n");

			move_to_next_chunk(chunkstr,saved_chunks);
			return strdup("");
		}
	}



    fprintf(mylog,"finding non-empty string\n");
    fflush(mylog);

    wlen = strnlen(chunkstr->p,len);

    if(wlen < len)
	{
		// String all contained in this chunk
		out = malloc(wlen + 1);
		memcpy(out, chunkstr->p, wlen + 1); // include the null byte
		fprintf(mylog,"string all from this chunk: %s\n",chunkstr->p);
		fflush(mylog);
		chunkstr->p += wlen + 1;  // skip to start of next string.
		return out;
	}

    out = malloc(wlen + 1);
	memcpy(out, chunkstr->p, wlen);
	out[wlen] = '\0';
	chunkstr->p += wlen;
	fprintf(mylog,"partial string: %s\n",out);
	fflush(mylog);


	if(chunkstr->p - chunkstr->chunk->data >= chunkstr->chunk->hdr.len)
		fprintf(mylog,"time for next chunk\n");

    while(chunkstr->p)
    {

        move_to_next_chunk(chunkstr,saved_chunks);
        if (chunkstr->p == NULL)
        	break;
        len = chunkstr->chunk->hdr.len - (chunkstr->p - chunkstr->chunk->data);

        wlen = strnlen(chunkstr->p,len);

        /* Write OK, don't forget to account for the trailing 0 */
        if(wlen < len)
        {
        	// Remainder of String all contained in this chunk
        	out = realloc(out, strlen(out) +  wlen + 1);
        	strncat(out, chunkstr->p, wlen + 1); // include the null byte

        	//memcpy(outstr, chunkstr->p, wlen + 1); // include the null byte
        	fprintf(mylog,"new fragment string: %s\n",chunkstr->p);
        	fprintf(mylog,"new  string: %s\n",out);
        	fflush(mylog);
            chunkstr->p += wlen + 1;  // skip to start of next string.
            fprintf(mylog,"next string: %s\n",chunkstr->p);
            return out;
        }
        else
        {

        	int newlen = strlen(out) + wlen;
        	out = realloc(out, newlen + 1);
        	strncat(out, chunkstr->p, wlen);
        	out[newlen] = '\0';
        	//memcpy(outstr, chunkstr->p, wlen);
        	//outstr[wlen] = '\0';
        	fprintf(mylog,"partial string: %s\n",out);
        	        	        	fflush(mylog);

            chunkstr->p += wlen;
            if(chunkstr->p - chunkstr->chunk->data >= chunkstr->chunk->hdr.len)
            	fprintf(mylog,"time for next chunk\n");
        }
    }

    return out;
}

#if USE_EMAIL
const char *
messagebody_cb(void **buf, int *len, void *arg)
{

	const GpErrorData *errorData = (GpErrorData *)arg;



	const char * message = "Return-Path: <cmcdevitt@greenplum.com>\r\n"
		"Subject: LibESMTP test mail\r\n"
		"MIME-Version: 1.0\r\n"
		"Content-Type: text/plain;\r\n"
		"  charset=iso-8859-1\r\n"
		"Content-Transfer-Encoding: 7bit\r\n\r\n"

		"Hello world!\r\n"

		"\r\n\r\nEnd of test---\r\n";

	if (errorData && errorData->username)
	{
		if ( strlen(errorData->username) > 255 )
			elog(ERROR,"Something strange");
	}
	*len = strlen(message);
	return message;
}

static int authinteract(auth_client_request_t request, char **result,
		int fields, void *arg)
{
	int i;

	for (i = 0; i < fields; i++)
	{
		if (request[i].flags & AUTH_PASS)
		{
			result[i] = "password";
			//result[i] = my_auth->password;
		}
		else if (request[i].flags & AUTH_USER)
		{
			result[i] = "chuck";
			//result[i] = my_auth->username;
		}
		else if (result[i] == NULL)
		{
			return 0;
		}
	}

	return 1;

}

/*
 * We currently just return 1 to keep libesmtp happy
 * Will turn this into a proper event handler some day
 */
static void event_cb(smtp_session_t session, int event_no, void *arg, ...)
{
	va_list alist;
	int *ok;
	long foo;
	int bar;

	va_start(alist, arg);

	switch (event_no)
	{

	case SMTP_EV_WEAK_CIPHER:

		bar = va_arg(alist, long);
		ok = va_arg(alist, int*);
		*ok = 1;
		break;

	case SMTP_EV_INVALID_PEER_CERTIFICATE:

		foo = va_arg(alist, long);
		ok = va_arg(alist, int*);
		*ok = 1;
		break;

	case SMTP_EV_NO_PEER_CERTIFICATE:
	case SMTP_EV_WRONG_PEER_CERTIFICATE:
	case SMTP_EV_NO_CLIENT_CERTIFICATE:

		ok = va_arg(alist, int*);
		*ok = 1;
		break;

	case SMTP_EV_CONNECT:
	case SMTP_EV_MAILSTATUS:
	case SMTP_EV_RCPTSTATUS:
	case SMTP_EV_MESSAGEDATA:
	case SMTP_EV_MESSAGESENT:
	case SMTP_EV_DISCONNECT:
	default:

		break;

	}

	va_end(alist);

}
#endif

