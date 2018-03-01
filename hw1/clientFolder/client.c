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
#include <errno.h>
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

// following function from
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
	ts.tv_nsec = 200000;

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

	printf("Sent %d bytes to remote host with sequence number %d (3 times). And lastflag %d\n", nbytes, global_seqnum_out, buffer[4]);

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

int main (int argc, char * argv[])
{
	bzero(global_buffer,sizeof(global_buffer));

	int nbytes;                             // number of bytes send by sendto()
	int sock;                               //this will be our socket

	struct sockaddr_in remote;              //"Internet socket address structure"

	if (argc < 3)
	{
		printf("USAGE:  <server_ip> <server_port>\n");
		exit(1);
	}

	/******************
	  Here we populate a sockaddr_in struct with
	  information regarding where we'd like to send our packet 
	  i.e the Server.
	 ******************/
	bzero(&remote,sizeof(remote));               //zero the struct
	remote.sin_family = AF_INET;                 //address family
	remote.sin_port = htons(atoi(argv[2]));      //sets port to network byte order
	remote.sin_addr.s_addr = inet_addr(argv[1]); //sets remote IP address

	//Causes the system to create a generic socket of type UDP (datagram)
	if ((sock = socket(PF_INET, SOCK_DGRAM, 0)) < 0)
	{
		printf("unable to create socket");
	}

	char cmd[MAXBUFSIZE];
	size_t cmd_len;
	struct sockaddr_in from_addr;
	int addr_length = sizeof(struct sockaddr);
	char response[MAXBUFSIZE];
	while (1)
	{
		bzero(response,sizeof(response));
		printf("Please write a command (get, put, delete, ls, or exit): ");

		// place command at index 5 because other spots will have management info
		char c;
		cmd_len = 5;
		while ((c = getchar()) != '\n' && 1+cmd_len < MAXBUFSIZE)
		{
			cmd[cmd_len] = c;
			cmd_len++;
		}
		cmd[cmd_len] = '\0';
		cmd_len++;

		/******************
		  sendto() sends immediately.  
		  it will report an error if the message fails to leave the computer
		  however, with UDP, there is no error if the message is lost in the network once it leaves the computer.
		 ******************/
		nbytes = send_message(sock, cmd, cmd_len, remote);
		if (nbytes == -1)
		{
			printf("Failed to send command. Errno: %s\n", strerror(errno));
		}

		if (strcmp(&cmd[5], "exit") == 0)
		{
			printf("Sent exit command to server. Exiting client as well.\n");
			break;
		}
		else if (strcmp(&cmd[5], "ls") == 0)
		{
			printf("Requested remote server's local directory...\n");
			nbytes = receive_message(sock, response, &from_addr, &addr_length);
			printf("Here it is!\n%s", &response[5]);

		}
		else
		{
			// find separation character for subcommands
			int cmd_offset = 5;
			while (cmd[cmd_offset] != ' ' && cmd_offset < nbytes-1)
				cmd_offset++;

			if (cmd_offset == nbytes-1)
			{
				printf("Sent command '%s' was not understood by server.\n", &cmd[5]);
				continue;
			}

			// "delete" or "get" or "put"
			char subcmd1[cmd_offset+1-5];
			memcpy(subcmd1, &cmd[5], cmd_offset);
			subcmd1[cmd_offset-5] = '\0';

			// filename
			char subcmd2[cmd_len-cmd_offset];
			memcpy(subcmd2, &cmd[cmd_offset+1], cmd_len-cmd_offset-1);
			subcmd2[cmd_len-cmd_offset-1] = '\0';

			if (strcmp(subcmd1, "delete") == 0)
			{
				printf("Sent delete request for file '%s'.\n", subcmd2);
			}
			else if (strcmp(subcmd1, "get") == 0)
			{
				printf("Getting file '%s'...\n", subcmd2);
				receive_file(sock, subcmd2);
			}
			else if (strcmp(subcmd1, "put") == 0)
			{
				printf("Sending file '%s'...\n", subcmd2);
				send_file(sock, remote, subcmd2);
			}
			else
			{
				printf("Sent command '%s' was not understood by server.\n", &cmd[5]);
				continue;
			}
		}
	}

	close(sock);
}

