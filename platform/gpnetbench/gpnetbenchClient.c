#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <time.h>

#define INIT_RETRIES 5
void send_buffer(int fd, char* buffer, int bytes);

void usage()
{
	fprintf(stdout, "usage: gpnetbench -p PORT -H HOST [-d DURATION] [-b KB] [-h]\n");
	fprintf(stdout, "where\n");
	fprintf(stdout, "       PORT is the port to connect to for the server\n");
	fprintf(stdout, "       HOST is the hostname to connect to for the server\n");
	fprintf(stdout, "       DURATION is the number of seconds to sample the network, where the default is 60\n");
	fprintf(stdout, "       KB is the size of the send buffer in kilobytes, where the default is 32\n");
	fprintf(stdout, "       -h shows this help message\n");
}

int main(int argc, char** argv)
{
	int socketFd;
	int retVal;
	int c;
	int i;
	int serverPort = 0;
	int duration = 60;
	int actual_duration;
	char* hostname = NULL;
	char* sendBuffer = NULL;
	int kilobytesBufSize = 32;
	int bytesBufSize;
	struct sockaddr_in address;
	struct hostent* host_entry;
	time_t start_time;
	time_t end_time;
	unsigned int buffers_sent = 0;
	double megaBytesSent;
	double megaBytesPerSecond;

	while ((c = getopt (argc, argv, "hH:p:d:b:")) != -1)
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
			case 'H':
				hostname = optarg;
				break;
			case 'h':
			case '?':
        	default:
				usage();
				return 1;
		}
	}

	if (!serverPort)
	{
		fprintf(stdout, "-p port not specified\n");
		usage();
		return 1;
	}
	if (!hostname)
	{
		fprintf(stdout, "-H hostname not specified\n");
		usage();
		return 1;
	}

	// validate a sensible value for duration
	if (duration < 10 || duration > 3600)
	{
		fprintf(stdout, "duration must be between 10 and 3600 seconds\n");
		return 1;
	}

	// validate a sensible value for buffer size
	if (kilobytesBufSize < 1 || kilobytesBufSize > 10240)
	{
		fprintf(stdout, "buffer size for sending must be between 1 and 10240 KB\n");
		return 1;
	}
	bytesBufSize = kilobytesBufSize * 1024;

	sendBuffer = malloc(bytesBufSize);
	memset(sendBuffer, 0, bytesBufSize);

	socketFd = socket(PF_INET, SOCK_STREAM, 0); 

	if (socketFd < 0)
	{ 
		fprintf(stdout, "socket call failed\n");
		return 1;
	}   

	host_entry = gethostbyname(hostname);
	memset(&address, 0, sizeof(struct sockaddr_in));
	address.sin_family = AF_INET;
	memcpy((char *)&address.sin_addr,(char *)host_entry->h_addr, host_entry->h_length);
	address.sin_port = htons(serverPort);

	for (i = 0; i < INIT_RETRIES; ++i)
	{
    		retVal = connect(socketFd,(struct sockaddr *)&address, sizeof(address));
    		if (retVal == 0)
			break;
		sleep(1);
	}

	if (retVal < 0)
	{
		fprintf(stdout, "Could not connect to server after %d retries\n", INIT_RETRIES);
		return 1;
	}
	printf("Connected to server\n");

	start_time = time(NULL);
	end_time = start_time + duration;
	while (time(NULL) < end_time)
	{
		send_buffer(socketFd, sendBuffer, bytesBufSize);
		buffers_sent++;
	}

	actual_duration = time(NULL) - start_time;
	megaBytesSent = buffers_sent * (double)bytesBufSize / (1024.0*1024.0);
	megaBytesPerSecond = megaBytesSent / actual_duration;

	printf("duration = %d MB sent = %lf MB/sec = %lf\n", actual_duration, megaBytesSent, megaBytesPerSecond);

	return 0;
}

void send_buffer(int fd, char* buffer, int bytes)
{
	size_t retval;

	while(bytes > 0)
	{
		retval = send(fd, buffer, bytes, 0);
		if (retval < 0)
		{
			perror("error on send call");
			exit(1);
		}
		if (retval > bytes)
		{
			fprintf(stdout, "unexpected large return code from send %d with only %d bytes in send buffer\n", (int)retval, bytes);
		}

		// advance the  buffer by number of bytes sent and reduce number of bytes remaining to be sent
		bytes -= retval;
		buffer += retval;
	}
}



