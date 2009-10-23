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
const PipeProtoChunk * saved_chunks;
static char * get_str_from_chunk(CSVChunkStr *chunkstr);
static const char * messagebody_cb(void **buf, int *len, void *arg);
static int authinteract(auth_client_request_t request, char **result,
		int fields, void *arg);
static void event_cb(smtp_session_t session, int event_no, void *arg, ...);
#endif
FILE * mylog = NULL;

int
send_alert_from_chunks(const PipeProtoChunk *chunk, const PipeProtoChunk * saved_chunks_in)
{
	static bool firsttime = true;
	GpErrorData errorData;

	GpErrorDataFixFields *pfixed = (GpErrorDataFixFields *) (chunk->data);
	CSVChunkStr chunkstr = { chunk, chunk->data + sizeof(GpErrorDataFixFields) };

	if (chunk==NULL)
		return -1;
	pqsignal(SIGSEGV, SIG_DFL);
	if (firsttime)
	{
		sleep(20);
		firsttime = false;
	}
	if (mylog == NULL)
		mylog = fopen("/tmp/mylog.log","a");
	fprintf(mylog," In send_alert_from_chunks\n");
	fflush(mylog);

	if (chunk->hdr.log_format != 'c')
		elog(ERROR,"send_alert_from_chunks only works when CSV logging is enabled");
	saved_chunks = saved_chunks_in;

	memset(&errorData,0,sizeof(errorData));

	memcpy(&errorData.fix_fields, pfixed, sizeof(GpErrorDataFixFields));

	errorData.username = get_str_from_chunk(&chunkstr);
	fprintf(mylog,"%s\n",errorData.username);
	errorData.databasename = get_str_from_chunk(&chunkstr);
	errorData.remote_host = get_str_from_chunk(&chunkstr);
	errorData.remote_port = get_str_from_chunk(&chunkstr);
	errorData.error_severity = get_str_from_chunk(&chunkstr);
	errorData.sql_state = get_str_from_chunk(&chunkstr);
	errorData.error_message = get_str_from_chunk(&chunkstr);
	fprintf(mylog,"%s: %s\n",errorData.error_severity, errorData.error_message);

	// Don't forget to free them!



	return -1; //send_alert(&errorData);

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



static char *
get_str_from_chunk(CSVChunkStr *chunkstr)
{
    int wlen = 0;
    int len = 0;
    char * out = NULL;
    char * outstr = NULL;


    if (chunkstr->chunk != NULL)
	{
		len = chunkstr->chunk->hdr.len - (chunkstr->p - chunkstr->chunk->data);

		/* Check if the string is an empty string */
		if (len > 0 && chunkstr->p[0] == '\0')
			return strdup("");

		if (len == 0 && chunkstr->chunk->hdr.next >= 0)
		{
			const PipeProtoChunk *next_chunk = &saved_chunks[chunkstr->chunk->hdr.next];
			if (next_chunk->hdr.len > 0 && next_chunk->data[0] == '\0')
				return strdup("");
		}
	}
	else
	{
		if(chunkstr->p != NULL)
			fprintf(mylog,"Bug in send_alert:  chunkstr->p != NULL");
		return strdup("");
	}

    if (chunkstr->p == NULL)
    	return strdup("");


    fprintf(mylog,"finding non-empty string\n");
    while(chunkstr->p)
    {
        bool done = true;
        wlen = strlen(chunkstr->p);

        /* Write OK, don't forget to account for the trailing 0 */
        if(wlen < len)
        {
        	out = malloc(wlen + 1);
        	outstr = out;
        	memcpy(outstr, chunkstr->p, wlen + 1); // include the null byte
        	fprintf(mylog,"string: %s\n",chunkstr->p);
        	fflush(mylog);
            done = true;
            chunkstr->p += wlen + 1;  // skip to next string.
        }
        else
        {
        	fprintf(mylog,"partial string: %s\n",chunkstr->p);
        	        	fflush(mylog);
        	out = malloc(wlen + 2);
        	outstr = out;
        	memcpy(outstr, chunkstr->p, wlen);
        	outstr[wlen] = '\0';
        	outstr += wlen;
            chunkstr->p += wlen;
            done = true; // not really
        }

        if(chunkstr->p - chunkstr->chunk->data >= chunkstr->chunk->hdr.len)
        {
            /* switch to next chunk */
            if(chunkstr->chunk->hdr.next >= 0)
            {
                chunkstr->chunk = &saved_chunks[chunkstr->chunk->hdr.next];
                chunkstr->p = chunkstr->chunk->data;
                len = chunkstr->chunk->hdr.len - (chunkstr->p - chunkstr->chunk->data);
            }
            else
            {
                chunkstr->chunk = NULL;
                chunkstr->p = NULL;
            }
        }

        if(done)
            break;
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
			//result[i] = my_auth->password;
		}
		else if (request[i].flags & AUTH_USER)
		{
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

