#include <gpcc_net.h>
#include <stdio.h>
#include <stdlib.h>

void opened(gpcc_net_conn_t conn, gpcc_net_header_t* to_peer, gpcc_net_header_t* from_peer)
{
    printf("----- opened --------------------------\n");
}

void accepted(gpcc_net_conn_t conn, gpcc_net_header_t* from_peer)
{
    char** p;
    char** q;
    printf("----- accepted --------------------------\n");
    printf("  > ");
    for (p = from_peer->argv; *p; p++) {
	printf("%s ", *p);
    }
    printf("\n");

    for (p = from_peer->hname, q = from_peer->hvalue; *p; p++, q++) {
	printf("  > %s: %s\n", *p, *q);
    }
    printf("\n");
    
    gpcc_net_close(conn);
}

int received(gpcc_net_conn_t conn, const char* buf, int buflen)
{
    printf("----- received --------------------------\n");
    return 0;
}

void closed(gpcc_net_conn_t conn)
{
    printf("----- closed --------------------------\n");
}


void checkerr(int e, const char* fline)
{
    if (e) {
	fprintf(stderr, "%s: error %d\n", fline, e);
	exit(1);
    }
}


int main()
{
    int e;

    e = apr_initialize();
    checkerr(e, FLINE);

    atexit(apr_terminate);

    e = gpcc_net_init(9000, opened, accepted, received, closed);
    checkerr(e, FLINE);
	
    printf("initialized\n");

    while (1) {
	static int i = 0;
	i++;
	printf("once %d\n", i);
	e = gpcc_net_once(1);
	checkerr(e, FLINE);
    }

    return 0;
}

