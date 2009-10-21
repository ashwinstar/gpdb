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

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "pgtime.h"
#include "utils/guc.h"
#include "postmaster/syslogger.h"
#include "postmaster/sendalert.h"

#if USE_EMAIL
#if USE_SSL
#include <openssl/ssl.h>
#endif
#include <auth-client.h>
#include <libesmtp.h>
#endif


#if USE_EMAIL
static const char * messagebody_cb(void **buf, int *len, void *arg);
static int authinteract(auth_client_request_t request, char **result,
		int fields, void *arg);
//static void event_cb(smtp_session_t session, int event_no, void *arg, ...);
#endif


int
send_alert(PipeProtoChunk *chunk)
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

	Assert(chunk->hdr.log_format == 'c');

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

	/* Do what's needed at application level to use authentication.
	 */
	authctx = auth_create_context();
	auth_set_mechanism_flags(authctx, AUTH_PLUGIN_PLAIN, 0);
	auth_set_interact_cb(authctx, authinteract, NULL);

	//  smtp_set_eventcb(session, event_cb, NULL);

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

	smtp_set_messagecb(message, messagebody_cb, chunk);

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

	return 0;
}

typedef struct CSVChunkStr
{
    PipeProtoChunk *chunk;
    char *p;
} CSVChunkStr;

static const char *
get_str_from_chunk(CSVChunkStr *chunkstr)
{
    //int wlen = 0;
    int len = 0;

    if (chunkstr->chunk != NULL)
	{
		len = chunkstr->chunk->hdr.len - (chunkstr->p - chunkstr->chunk->data);

		//if (len == 0 && chunkstr->chunk->hdr.next >= 0)
		//	PipeProtoChunk *next_chunk = &saved_chunks[chunkstr->chunk->hdr.next];
	}
	else
		Assert(chunkstr->p == NULL);

    if (chunkstr->p == NULL)
    	return "";
    else
    	return chunkstr->p;

#if 0
    while(chunkstr->p)
    {
        bool done = false;
        wlen = strlen(chunkstr->p);

        /* Write OK, don't forget to account for the trailing 0 */
        if(wlen < len)
        {
            done = true;
            chunkstr->p += wlen + 1;
        }
        else
            chunkstr->p += wlen;

        if(chunkstr->p - chunkstr->chunk->data == chunkstr->chunk->hdr.len)
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
#endif
}

#if USE_EMAIL
const char *
messagebody_cb(void **buf, int *len, void *arg)
{

	PipeProtoChunk *chunk = (PipeProtoChunk *)arg;


	//GpErrorDataFixFields *pfixed = (GpErrorDataFixFields *) (chunk->data);
	CSVChunkStr chunkstr = { chunk, chunk->data + sizeof(GpErrorDataFixFields) };

	const char * message = "Return-Path: <cmcdevitt@greenplum.com>\r\n"
		"Subject: LibESMTP test mail\r\n"
		"MIME-Version: 1.0\r\n"
		"Content-Type: text/plain;\r\n"
		"  charset=iso-8859-1\r\n"
		"Content-Transfer-Encoding: 7bit\r\n\r\n"

		"Hello world!\r\n"

		"\r\n\r\nEnd of test---\r\n";
	if (chunk==NULL)
	{
		*len = 0;
		return NULL;
	}
	get_str_from_chunk(&chunkstr);
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

#if 0
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
#endif

