#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

int main(int argc, char** argv)
{
	int socketFd;
	int c;
	int serverPort = 0;
	int duration = 60;
	char* hostname = NULL;
	int kilobytesBufSize = 32;

	while ((c = getopt (argc, argv, "h:p:d:b:")) != -1)
	{
		switch (c)
		{
			case 'p':
				serverPort = atoi(optarg);
				break;
			case 'd':
				duration = atoi(optarg);
				break;
			case 'b':
				kilobytesBufSize = atoi(optarg);
				break;
			case 'h':
				hostname = optarg;
				break;
			case '?':
        	default:
				fprintf(stdout, "usage: \n");
				return 1;
		}
	}

	if (!serverPort)
	{
		fprintf(stdout, "-p port not specified\n");
		return 1;
	}

	socketFd = socket(PF_INET, SOCK_STREAM, 0); 

	if (socketFd < 0){ 
		fprintf(stdout, "socket call failed\n");
		return 0;
	}   

	return 0;
}

