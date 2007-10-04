#include <gpcc_net.h>
#include <stdio.h>
#include <stdlib.h>


void checkerr(int e, const char* fline)
{
    if (e) {
	fprintf(stderr, "%s: error %d\n", fline, e);
	exit(1);
    }
}


void tick(apr_time_t now)
{
    static int start_sec = 0;
    int sec = apr_time_sec(now);
    if (! start_sec)
	start_sec = sec;
    sec -= start_sec;
    printf("tick: %d\n", sec);
}


void svr_opened(gpcc_net_conn_t conn, const char* peer_addr, gpcc_net_header_t* to_peer, gpcc_net_header_t* from_peer)
{
    printf("----- svr_opened --------------------------\n");
    gpcc_fatal(FLINE, "INTERNAL ERROR - server does not open\n");
}

void svr_accepted(gpcc_net_conn_t conn, const char* peer_addr, gpcc_net_header_t* from_peer)
{
    int e;
    char* s = "SERVER ACCEPTED\r\n\r\n";
    char** p;
    char** q;
    printf("----- svr_accepted --------------------------\n");
    printf("  peer_addr: %s\n", peer_addr);
    printf("  > ");
    for (p = from_peer->argv; *p; p++) {
	printf("%s ", *p);
    }
    printf("\n");

    for (p = from_peer->hname, q = from_peer->hvalue; *p; p++, q++) {
	printf("  > %s: %s\n", *p, *q);
    }
    printf("\n");
    
    /* send a header */
    e = gpcc_net_send(conn, s, strlen(s));
    checkerr(e, FLINE);
    /*gpcc_net_close(conn);*/
}

int svr_received(gpcc_net_conn_t conn, const char* buf, int buflen)
{
    int e;
    int i;
    printf("----- svr_received --------------------------\n");
    if (buflen <= 0) {
	gpcc_fatal(FLINE, "INTERNAL ERROR - buflen %d", buflen);
    }
    for (i = 0; i < buflen; i++) {
	putchar(buf[i]);
    }
    e = gpcc_net_send(conn, buf, buflen);
    checkerr(e, FLINE);
    return buflen;
}

void received_packet(gpcc_net_conn_t conn, gpcc_net_packet_t* pkt)
{
    printf("----- received packet--------------------------\n");
    gpcc_fatal(FLINE, "INTERNAL ERROR - this test does not exercise packet logic");
}



void svr_closed(gpcc_net_conn_t conn)
{
    printf("----- svr_closed --------------------------\n");
}


void do_send(gpcc_net_conn_t conn)
{
    static int count = 0;
    char buf[100];
    int e;
    sprintf(buf, " -- hello %d -- \n", ++count);
    e = gpcc_net_send(conn, buf, strlen(buf));
    checkerr(e, FLINE);
}

void clnt_opened(gpcc_net_conn_t conn, const char* peer_addr, gpcc_net_header_t* to_peer, gpcc_net_header_t* from_peer)
{
    printf("----- clnt_opened --------------------------\n");
    printf("   peer_addr: %s\n", peer_addr);
    do_send(conn);
}



void clnt_accepted(gpcc_net_conn_t conn, const char* peer_addr, gpcc_net_header_t* from_peer)
{
    printf("----- clnt_accepted --------------------------\n");
    gpcc_fatal(FLINE, "INTERNAL ERROR - client does not accept");
}

int clnt_received(gpcc_net_conn_t conn, const char* buf, int buflen)
{
    const char* q = buf + buflen;
    const char* p = buf;
    printf("----- clnt_received --------------------------\n");
    while (p < q) {
	if (q[-1] == '\n')
	    break;
	q--;
    }
    if (p < q) {
	int ch;
	while (p < q) {
	    ch = *p++;
	    putchar(ch);
	}
	printf("Press [RETURN] to send the next line: ");
	fflush(stdout);
	ch = getchar();
	do_send(conn);
    }
    return p - buf;
}

void clnt_closed(gpcc_net_conn_t conn)
{
    printf("----- clnt_closed --------------------------\n");
}



void usage(const char* pname)
{
    fprintf(stderr, "usage: %s [remote] port\n\n", pname);
    fprintf(stderr, "\tremote : Name or IP of remote host\n");
    fprintf(stderr, "\tport   : If 'server' is not specified, then this is the local port\n");
    fprintf(stderr, "\t         and %s will function as an echo server. \n", pname);
    fprintf(stderr, "\t         Otherwise, %s will function as a client, and sends some \n", pname);
    fprintf(stderr, "\t         strings to the remote server\n");
    exit(1);
}


int main(int argc, char* argv[])
{
    int e;
    char* svr = 0;
    int port = 0;

    if (argc == 3) {
	svr = argv[1];
	port = strtol(argv[2], 0, 0);
    }
    else if (argc == 2) {
	port = strtol(argv[1], 0, 0);
    }
    if (! (0 < port && port < (1 << 16)))
	usage(argv[0]);

    e = apr_initialize();
    checkerr(e, FLINE);

    atexit(apr_terminate);

    if (! svr) {
	e = gpcc_net_init(port, svr_opened, svr_accepted, svr_received, received_packet, svr_closed, tick);
	checkerr(e, FLINE);
	
	printf("Listening on port %d\n", port);
    }
    else {
	gpcc_net_conn_t conn;
	static char* av[] = {"HELLO", "1", "2", "3" };
	static char* hn[] = { "N1", "N2", "N3" };
	static char* hv[] = { "V1", "V2", "V3" };
	char addr[100];

	e = gpcc_net_init(0, clnt_opened, clnt_accepted, clnt_received, received_packet, clnt_closed, tick);
	checkerr(e, FLINE);

	sprintf(addr, "%s:%d", svr, port);
	printf("Connecting to %s\n", addr);
	e = gpcc_net_open(&conn, addr, 3, av, 3, hn, hv);
	checkerr(e, FLINE);
    }

    while (1) {
	e = gpcc_net_once(1);
	checkerr(e, FLINE);
    }

    return 0;
}

