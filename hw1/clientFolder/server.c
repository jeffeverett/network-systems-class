#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <dirent.h> 
#include <time.h>

#define MAXBUFSIZE 512
#define RETAIN_NUM 10000

char global_buffer[MAXBUFSIZE*RETAIN_NUM];
int global_buffer_sizes[RETAIN_NUM];
int global_index = 0;
uint32_t global_seqnum_in = 1;
uint32_t global_seqnum_out = 1;

// function prototypes
uint32_t extract_uint32_le(char *buffer);
void receive_file(int sock, char *filename);
void send_file(int sock, struct sockaddr_in remote, char *filename);
int send_message(int sock, char* buffer, int len, struct sockaddr_in remote);
int receive_message(int sock, char *buffer, struct sockaddr_in *remote, unsigned int *remote_length);

// following function based off of
// https://stackoverflow.com/questions/11134696/how-to-extract-4-bytes-of-integer-from-a-buffer-of-integers-in-c
uint32_t extract_uint32_le(char *buffer)
{
  const uint8_t b0 = buffer[3], b1 = buffer[2], b2 = buffer[1], b3 = buffer[0];

  return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
}

void receive_file(int sock, char *filename)
{
	char buffer[MAXBUFSIZE];

	FILE* fp = fopen(filename, "w");
	if (fp == NULL)
	{
		printf("Could not open file locally to write results.\n");
		return;
	}

	struct sockaddr_in from_addr;
	int addr_length = sizeof(struct sockaddr);
	uint8_t lastflag = 0x00;
	// continue receiving messages until the opposing server indicates that it is the last one
	while (lastflag == 0x00)
	{
		bzero(buffer,sizeof(char[MAXBUFSIZE]));
		int nbytes = receive_message(sock, buffer, &from_addr, &addr_length);

		int res = fwrite(&buffer[5], 1, nbytes-5, fp);
		if (res != nbytes-5)
		{
			printf("Failed to write to file (locally).\n");
			break;
		}

		// last datagram flag
		lastflag = (uint8_t) buffer[4];
	}
	fclose(fp);
}

void send_file(int sock, struct sockaddr_in remote, char *filename)
{
	char buffer[MAXBUFSIZE];

	FILE* fp = fopen(filename, "r");
	if (fp == NULL)
	{
		printf("Could not open requested file.\n");
		return;
	}

	uint8_t lastflag = 0x00;
	// continue receiving messages until the opposing server indicates that it is the last one
	while (lastflag == 0x00)
	{
		bzero(buffer,sizeof(char[MAXBUFSIZE]));

		// get sequence num from buffer
		uint32_t seqnum = extract_uint32_le(buffer);

		int nelems = fread(&buffer[5], 1, sizeof(buffer)-5, fp);
		if (nelems < sizeof(buffer)-5)
			lastflag = 0xFF;
		buffer[4] = lastflag;
		int nbytes = send_message(sock, buffer, nelems+5, remote);
	}
	fclose(fp);
}

int send_message(int sock, char* buffer, int len, struct sockaddr_in remote)
{
	// short delay so remote server doesn't get overwhelmed
	struct timespec ts, ts2;
	ts.tv_sec = 0;
	ts.tv_nsec = 1000000;

	nanosleep(&ts, &ts2);

	// store sequence number in first 4 bytes of buffer
	memcpy(buffer, &global_seqnum_out, 4);

	// send 3 times for reliable messaging
	int nbytes =  sendto(sock, buffer, len, 0, (struct sockaddr *)&remote, sizeof(remote));
	sendto(sock, buffer, len, 0, (struct sockaddr *)&remote, sizeof(remote));
	sendto(sock, buffer, len, 0, (struct sockaddr *)&remote, sizeof(remote));

	if (nbytes == -1)
	{
		printf("Error encountered while sending data: %s\n", strerror(errno));
	}

	printf("Sent %d bytes to remote host with sequence number %d (3 times).\n", nbytes, global_seqnum_out);

	global_seqnum_out++;

	return nbytes;
}

int receive_message(int sock, char *buffer, struct sockaddr_in *remote, unsigned int *remote_length)
{
	// at the beginning of each receive_message call, check if the desired message
	// was received out of order and is already stored in global_buffer
	for (int i = 0; i < RETAIN_NUM; i++)
	{
		if (extract_uint32_le(&global_buffer[i*MAXBUFSIZE]) == global_seqnum_in)
		{
			global_seqnum_in++;
			memcpy(buffer, &global_buffer[i*MAXBUFSIZE], sizeof(buffer));
			return global_buffer_sizes[i];
		}
	}


	// otherwise, continue to receive messages until the desired message is given
	int nbytes;

	while (1)
	{
		bzero(buffer,sizeof(char[MAXBUFSIZE]));

		nbytes = recvfrom(sock, buffer, sizeof(char[MAXBUFSIZE]), 0, (struct sockaddr *)remote, remote_length);

		// get sequence num from buffer
		uint32_t seqnum = extract_uint32_le(buffer);

		printf("Received %d bytes from remote host with sequence number %d.\n", nbytes, seqnum);

		if (global_seqnum_in == seqnum)
		{
			// the received message was the expected one
			global_seqnum_in++;
			return nbytes;
		}
		else if (seqnum < global_seqnum_in)
		{
			// this packet has already been received. discard it
			continue;
		}
		else
		{
			// this packet has arrived out of order. store it for later.
			memcpy(&global_buffer[MAXBUFSIZE*global_index], buffer, sizeof(buffer));
			global_buffer_sizes[global_index] = nbytes;
			global_index = (global_index+1) % RETAIN_NUM;
		}
	}
}


int main (int argc, char * argv[] )
{
	bzero(global_buffer,sizeof(global_buffer));

	int sock;                           //This will be our socket
	struct sockaddr_in sin, remote;     //"Internet socket address structure"
	unsigned int remote_length;         //length of the sockaddr_in structure
	int nbytes;                        //number of bytes we receive in our message
	char cmd[MAXBUFSIZE];             //a buffer to store our received message
	if (argc != 2)
	{
		printf ("USAGE:  <port>\n");
		exit(1);
	}

	/******************
	  This code populates the sockaddr_in struct with
	  the information about our socket
	 ******************/
	bzero(&sin,sizeof(sin));                    //zero the struct
	sin.sin_family = AF_INET;                   //address family
	sin.sin_port = htons(atoi(argv[1]));        //htons() sets the port # to network byte order
	sin.sin_addr.s_addr = INADDR_ANY;           //supplies the IP address of the local machine


	//Causes the system to create a generic socket of type UDP (datagram)
	if ((sock = socket(PF_INET, SOCK_DGRAM, 0)) < 0)
	{
		printf("unable to create socket");
	}


	/******************
	  Once we've created a socket, we must bind that socket to the 
	  local address and port we've supplied in the sockaddr_in struct
	 ******************/
	if (bind(sock, (struct sockaddr *)&sin, sizeof(sin)) < 0)
	{
		printf("unable to bind socket\n");
	}


	char response[MAXBUFSIZE];
	remote_length = sizeof(remote);
	while (1)
	{
		printf("Waiting for message from client.\n");

		bzero(cmd,sizeof(cmd));
		bzero(response,sizeof(response));

		//waits for an incoming message
		nbytes = receive_message(sock, cmd, &remote, &remote_length);
		if (strcmp(&cmd[5], "exit") == 0)
		{
			// server has been instructed to exit, so do so
			printf("'exit' command received. Shutting down.\n");
			break;
		}
		else if (strcmp(&cmd[5], "ls") == 0)
		{
			printf("'ls' command received. Sending list of current directory.\n");

			int response_offset = 5;
			// note: the following is based off of
			// https://stackoverflow.com/questions/4204666/how-to-list-files-in-a-directory-in-a-c-program
			// here, we assume that the contents of the directory will always fit into one message
			DIR *d;
			struct dirent *dir;
			d = opendir(".");
			if (d)
			{
				while ((dir = readdir(d)) != NULL)
				{
					memcpy(&response[response_offset], dir->d_name, strlen(dir->d_name));
					response_offset += strlen(dir->d_name);

					response[response_offset] = '\n';
					response_offset++;
				}

				closedir(d);
			}

			nbytes = send_message(sock, response, response_offset, remote);
		}
		else
		{
			// find separation character for subcommands
			int cmd_offset = 5;
			while (cmd[cmd_offset] != ' ' && cmd_offset < nbytes-1)
				cmd_offset++;

			if (cmd_offset == nbytes-1)
			{
				printf("Received command '%s'. Not understood.\n", &cmd[5]);
				continue;
			}

			// "delete" or "get" or "put"
			char subcmd1[cmd_offset+1-5];
			memcpy(subcmd1, &cmd[5], cmd_offset);
			subcmd1[cmd_offset-5] = '\0';

			// filename
			char subcmd2[nbytes-cmd_offset];
			memcpy(subcmd2, &cmd[cmd_offset+1], nbytes-cmd_offset-1);
			subcmd2[nbytes-cmd_offset-1] = '\0';

			if (strcmp(subcmd1, "delete") == 0)
			{
				printf("Received delete request for file '%s'. Deleting (if it exists).\n", subcmd2);
				remove(subcmd2);

			}
			else if (strcmp(subcmd1, "get") == 0)
			{
				printf("Received request for file '%s'. Sending file.\n", subcmd2);
				send_file(sock, remote, subcmd2);
			}
			else if (strcmp(subcmd1, "put") == 0)
			{
				printf("Received put request for file '%s'. Receiving file.\n", subcmd2);
				receive_file(sock, subcmd2);
			}
			else
			{
				printf("Received command '%s'. Not understood.\n", &cmd[5]);
				continue;
			}
		}
	}

	close(sock);
}

