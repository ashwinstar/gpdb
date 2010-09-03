#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>

int main(int argc, char** argv)
{
	int socketFd;

	socketFd = socket(PF_INET, SOCK_STREAM, 0); 

	if (socketFd < 0){ 
		fprintf(stdout, "socket call failed\n");
		return 0;
	}   

	return 0;
}

