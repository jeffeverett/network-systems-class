#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#define MAX_CONF_LINE_SIZE 255
#define MAX_POST_DATA 2000
#define MAX_HEADER_LINE_SIZE 500
#define MAX_STATUS_SIZE 50
#define MAX_URI 400
#define MAX_ERROR_SIZE 200
#define MAX_DEFAULT_PAGES 10
#define MAX_CONTENT_TYPES 20
#define BACKLOG 20
#define BUFFER_SIZE 50000
# define CACHE_SIZE 200


int sockfd;
char blocked_ips[20][100];
char blocked_ip_idx = 0;
char blocked_hosts[20][100];
char blocked_host_idx = 0;

int last_cleanup = 0;

char hashes[CACHE_SIZE][32];
clock_t entered_cache[CACHE_SIZE];
int occupied[CACHE_SIZE];

long cache_timeout;

void sig_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM)
    {
        printf("Shutdown signal received. Closing sockets and then exiting.\n");
        close(sockfd);
        exit(0);
    }
}

char *get_ext(char *uri) {
    char *dot = strrchr(uri, '.');
    if(!dot || dot == uri) return "";
    return dot + 1;
}

void remove_newline(char *buff)
{
    // remove newline from buffer--we don't need or want it
    char *newline;
    if ((newline = strchr(buff, '\n')) != NULL)
        *newline = '\0';
}

// http://www.programmingsimplified.com/c/program/c-program-change-case
void lower_string(char s[]) {
   int c = 0;
 
   while (s[c] != '\0') {
      if (s[c] >= 'A' && s[c] <= 'Z') {
         s[c] = s[c] + 32;
      }
      c++;
   }
}

void send_error(int new_sockfd, char *version, char *status, char *error_response)
{
    char buffer[BUFFER_SIZE];
    bzero(buffer, BUFFER_SIZE);

    char size_str[20];
    sprintf(size_str, "%d", (int)strlen(error_response));

    strcpy(buffer, "HTTP/");
    strcat(buffer, version);
    strcat(buffer, " ");
    strcat(buffer, status);
    strcat(buffer, "\nContent-Type: ");
    strcat(buffer, "text/html");
    strcat(buffer, "\nContent-Length: ");
    strcat(buffer, size_str);
    strcat(buffer, "\n\n");
    strcat(buffer, error_response);
    send(new_sockfd, buffer, strlen(buffer), 0);

    free(error_response);
    free(status);
}

void send_400_error(int new_sockfd, char *version, char* reason, char *method)
{
    char *error_response = malloc(MAX_ERROR_SIZE);
    if (strcmp(reason, "method") == 0)
    {
        strcpy(error_response, "<html><body>400 Bad Request. Reason: Invalid Method: ");
        strcat(error_response, method);
        strcat(error_response, "</body></html>");
    }
    else if (strcmp(reason, "method") == 0)
    {
        strcpy(error_response, "<html><body>400 Bad Request Reason: Invalid HTTP-Version: ");
        strcat(error_response, method);
        strcat(error_response, "</body></html>");
    }
    char *status = malloc(MAX_STATUS_SIZE);
    strcpy(status, "400 Bad Request");
    send_error(new_sockfd, version, status, error_response);
}

void send_403_error(int new_sockfd, char *version, char *uri)
{
    char *error_response = malloc(MAX_ERROR_SIZE);
    strcpy(error_response, "<html><body>403 Forbidden. Following host/IP is forbidden: ");
    strcat(error_response, uri);
    strcat(error_response, "</body></html>");
    char *status = malloc(MAX_STATUS_SIZE);
    strcpy(status, "403 Forbidden");
    send_error(new_sockfd, version, status, error_response);
}

void send_404_error(int new_sockfd, char *version, char *uri)
{
    char *error_response = malloc(MAX_ERROR_SIZE);
    strcpy(error_response, "<html><body>404 Not Found. Reason: URL does not exist: ");
    strcat(error_response, uri);
    strcat(error_response, "</body></html>");
    char *status = malloc(MAX_STATUS_SIZE);
    strcpy(status, "404 Not Found");
    send_error(new_sockfd, version, status, error_response);
}

void send_501_error(int new_sockfd, char *version, char *method)
{
    char *error_response = malloc(MAX_ERROR_SIZE);
    strcpy(error_response, "<html><body>501 Not Implemented. Reason: ");
    strcat(error_response, method);
    strcat(error_response, "</body></html>");
    char *status = malloc(MAX_STATUS_SIZE);
    strcpy(status, "501 Not Implemented");
    send_error(new_sockfd, version, status, error_response);
}

void send_500_error(int new_sockfd, char *version, char *error)
{
    char *error_response = malloc(MAX_ERROR_SIZE);
    strcpy(error_response, "<html><body>500 Internal Server Error: ");
    strcat(error_response, error);
    strcat(error_response, "</body></html>");
    char *status = malloc(MAX_STATUS_SIZE);
    strcpy(status, "500 Internal Server Error");
    send_error(new_sockfd, version, status, error_response);
}

int check_cachedaddr(char *host, struct sockaddr_in *dest)
{
    FILE *fcacheaddr = fopen("cached_addresses.txt", "r");
    if (!fcacheaddr)
    {
        perror("Cannot open cached addresses (to read)");
        return 0;
    }

    char *line;
    ssize_t read;
    size_t len = 0;
    while ((read = getline(&line, &len, fcacheaddr)) != -1)
    {
        char host_line[500];
        char ip_line[500];
        char *comma = strchr(line, ',');
        *comma = '\0';
        strcpy(host_line, line);
        strcpy(ip_line, comma+1);
        // remove newline from ip
        ip_line[strlen(ip_line)-1] = '\0';

        printf("%s,%s\n",host_line,ip_line);

        if (strcmp(host_line, host) == 0)
        {
            inet_pton(AF_INET, ip_line, &dest->sin_addr);
            fclose(fcacheaddr);
            return 1;
        }
    }
    fclose(fcacheaddr);

    return 0;
}

void add_cachedaddr(char *host, struct sockaddr_in *addr)
{
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip_str, INET_ADDRSTRLEN);

    FILE *fcacheaddr = fopen("cached_addresses.txt", "a");
    if (!fcacheaddr)
    {
        perror("Cannot open cached addresses (to write)");
        pthread_exit(0);
    }

    char line[500];
    strcpy(line, host);
    strcat(line, ",");
    strcat(line, ip_str);
    strcat(line, "\n");

    fseek(fcacheaddr, 0, SEEK_END);
    fwrite(line, 1, strlen(line), fcacheaddr);
    fclose(fcacheaddr);
}

void send_file(int new_sockfd, char *hash, char *version)
{
    char mime[MAX_CONF_LINE_SIZE];
    bzero(mime, MAX_CONF_LINE_SIZE);

    // determine the file size of the requested file
    FILE *fp = fopen(hash, "r");
    if (!fp)
    {
        send_500_error(new_sockfd, version, "Could not open specified file.");
        return;
    }
    fseek(fp, 0L, SEEK_END);
    int size = ftell(fp);
    rewind(fp);

    char size_str[200];
    sprintf(size_str, "%d", size);

    // actually send the headers/file
    char buffer[BUFFER_SIZE];
    bzero(buffer, BUFFER_SIZE);

    strcpy(buffer, "HTTP/");
    strcat(buffer, version);
    strcat(buffer, " 200 OK");
    strcat(buffer, "\nContent-Length: ");
    strcat(buffer, size_str);
    strcat(buffer, "\nConnection: close");

    strcat(buffer, "\n\n");

    printf("%s\n", buffer);
    send(new_sockfd, buffer, strlen(buffer), 0);

    int nbytes = 0;
    while (!feof(fp))
    {
        nbytes = fread(buffer, 1, BUFFER_SIZE, fp);
        if (nbytes > 0)
            send(new_sockfd, buffer, nbytes, 0);
        printf("%s", buffer);
    }
}

void *handle_connection(void *new_sockfd_p)
{
    int new_sockfd = *((int*)new_sockfd_p);

    char buffer[BUFFER_SIZE];
    char method[5];
    char version[4];
    char uri[MAX_HEADER_LINE_SIZE];
    char header_line[MAX_HEADER_LINE_SIZE];
    int content_size = -1;

    printf("-------------------------------------\nServing with sockfd: %d\n", new_sockfd);
    int nbytes = recv(new_sockfd, buffer, BUFFER_SIZE-1, 0);
    if (nbytes <= 0)
    {
        perror("error reading request");
        pthread_exit(0);
    }

    // make buffer terminate with null char
    buffer[nbytes] = '\0';

    printf("%s", buffer);

    // note: http connections apparently use \r\n instead of \n
    ssize_t read;
    char line[500];

    char *eol = strchr(buffer, '\n');
    *eol = '\0';
    strcpy(line, buffer);
    *eol = '\n';

    sscanf(line, "%s %s HTTP/%s\r\n", method, uri, version);

    char host[250];
    sscanf(uri, "http://%s", host);
    char *slash = strchr(host, '/');
    if (slash)
        *slash = '\0';

    printf("host: %s, uri: %s\n", host, uri);
    lower_string(method);

    // block host if blocked
    for (int i = 0; i < blocked_host_idx; i++)
    {
        if (strcmp(host, blocked_hosts[i]) == 0)
        {
            send_403_error(new_sockfd, version, uri);
            pthread_exit(0);
        }
    }

    // ensure proper version of HTTP is used
    if (strcmp(version, "1.1") != 0 && strcmp(version, "1.0") != 0)
    {
        send_400_error(new_sockfd, version, "version", version);
        pthread_exit(0);
    }

    // make sure method is acceptable
    if (strcmp(method, "get") != 0)
    {
        send_501_error(new_sockfd, version, method);
        pthread_exit(0);
    }

    // compute md5sum hash string
    char command[strlen("echo ") + strlen(uri) + strlen(" | md5sum") + 1];
    strcpy(command, "echo ");
    strcat(command, uri);
    strcat(command, " | md5sum");

    FILE *pp = popen(command, "r");
    if (!pp)
    {
        perror("Failed to run md5sum command");
        pthread_exit(0);
    }

    char hex_string[33];
    nbytes = fread(hex_string, 1, 32, pp);
    if (nbytes < 32)
    {
        printf("Failed to read results of md5sum command.\n");
        pthread_exit(0);
    }
    hex_string[32] = '\0';

    int use_cached = 0;
    if (access(hex_string,F_OK) != -1)
    {
        for (int i = 0; i < CACHE_SIZE; i++)
        {
            if (occupied[1] && strcmp(hashes[i], hex_string) == 0 &&
                (clock()-entered_cache[i])/CLOCKS_PER_SEC>cache_timeout)
            {
                use_cached = 1;
            }
        }
    }

    if (use_cached)
    {
        printf("sending file from cache\n");

        FILE *fcache = fopen(hex_string, "r");
        if (!fcache)
        {
            perror("could not open cached file.");
            pthread_exit(0);
        }

        while (fgets(buffer, BUFFER_SIZE, fcache))
        {
            send(new_sockfd, buffer, nbytes, 0);
        }
    }
    else
    {
        printf("not sending file from cache\n");

        struct sockaddr_in remote_addr;

        // check stored hosts for host
        int had_ip = check_cachedaddr(host, &remote_addr);
        had_ip = 0;
        if (had_ip)
        {
            printf("Using cached address.\n");
            remote_addr.sin_family = AF_INET;
            remote_addr.sin_port = htons(80);
        }
        else
        {
            printf("Not using cached address.\n");
            // create new socket to connect to requested server
            struct addrinfo hints;
            hints.ai_flags = 0;
            hints.ai_family = INADDR_ANY;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = 0;
            struct addrinfo *res;

            int rc = getaddrinfo(host, "80", &hints, &res);
            if (rc != 0)
            {
                printf("getaddrinfo call failed: %s.\n", gai_strerror(rc));
                pthread_exit(0);
            }
            remote_addr = *((struct sockaddr_in*) res->ai_addr);

            // store this address
            add_cachedaddr(host, &remote_addr);
        }

        // block ip address if necessary
        char ip_str[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &(remote_addr.sin_addr), ip_str, INET_ADDRSTRLEN) == 0)
        {
            perror("Could not convert IP to presentation format");
            pthread_exit(0);
        }
        for (int i = 0; i < blocked_ip_idx; i++)
        {
            if (strcmp(blocked_ips[i], ip_str) == 0)
            {
                send_403_error(new_sockfd, version, uri);
                pthread_exit(0);
            }
        }

        // create socket and connect
        int remote_sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (remote_sockfd == -1)
        {
            perror("creation of remote socket failed");
            pthread_exit(0);
        }
        socklen_t addrlen = sizeof(struct sockaddr);
        if (connect(remote_sockfd, (struct sockaddr*)&remote_addr, addrlen) != 0)
        {
            printf("%d", errno);
            perror("connect call failed");
            pthread_exit(0);
        }

        // send this server the GET line
        printf("Sending original request to remote server:\n%s", buffer);
        if (send(remote_sockfd, buffer, strlen(buffer), 0) == -1)
        {
            printf("relay of HTTP GET failed: %s\n", strerror(errno));
            pthread_exit(0);
        }

        // add file to cache at same time as sending back to client
        FILE *fcache = fopen(hex_string, "w");
        if (!fcache)
        {
            perror("Failed to open cache write file");
            pthread_exit(0);
        }

        printf("Created connection to %s. Will now echo received data to client.\n", uri);
        int headers_done = 0;
        while ((nbytes = recv(remote_sockfd, buffer, BUFFER_SIZE-1, 0)) > 0)
        {
            // make buffer terminate with null char
            buffer[nbytes] = '\0';

            char *remainder = buffer;
            if (!headers_done)
            {
                char *gap = strstr(buffer, "\r\n\r\n");
                if (gap)
                {
                    *gap = '\0';
                    remainder = gap+4;
                    headers_done = 1;
                }
                else
                {
                    continue;
                }
            }
            printf("remainder: %s\n", remainder);

            // cache part
            int amt_to_write = nbytes-(remainder-buffer);
            int wrote = fwrite(remainder, 1, amt_to_write, fcache);
            if (wrote != amt_to_write)
            {
                printf("error writing to cache file\n");
                pthread_exit(0);
            }
        }
        fclose(fcache);

        // now send cached file
        send_file(new_sockfd, hex_string, version);
    }

    printf("closing socket\n");
    close(new_sockfd);
}

int main(int argc, char **argv)
{
    setbuf(stdout,NULL);
    setbuf(stderr,NULL);

    if (argc != 3)
    {
        printf("Format: webproxy <port> <cache_timeout>\n");
        exit(1);
    }
    char *timeout_str = argv[2];
    cache_timeout = strtol(timeout_str, 0, 10);

    // read in from blocked file
    FILE *fp = fopen("blocked.txt", "r");
    if (!fp)
    {
        printf("Cannot open list of blocked hostnames/IPs.\n");
        exit(1);
    }
    char *line;
    ssize_t read;
    size_t len = 0;
    int reading_ips = 0;
    while ((read = getline(&line, &len, fp)) != -1)
    {
        if (strcmp("<Hostnames>", line) == 0)
        {
            reading_ips = 0;
        }
        else if (strcmp("<IPs>", line) == 0)
        {
            reading_ips = 1;
        }
        else
        {
            if (reading_ips)
            {
                strcpy(blocked_ips[blocked_ip_idx], line);
                blocked_ip_idx++;
            }
            else
            {
                strcpy(blocked_hosts[blocked_host_idx], line);
                blocked_host_idx++;
            }
        }
    }

    // handle signals in order to properly close sockfds
    if (signal(SIGINT, sig_handler) == SIG_ERR)
        printf("Can't catch SIGINT.\n");
    if (signal(SIGTERM, sig_handler) == SIG_ERR)
        printf("Can't catch SIGTERM.\n");

    char *port_str = argv[1];
    long port = strtol(port_str, 0, 10);

    // create socket, bind, and listen
    struct sockaddr_in sin, remote;
    unsigned int remote_length;

    bzero(&sin, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = INADDR_ANY;

    if ((sockfd = socket(PF_INET, SOCK_STREAM, 0)) < 0)
    {
        printf("Unable to create socket. Exiting.\n");
        exit(1);
    }

    if (bind(sockfd, (struct sockaddr*)&sin, sizeof(sin)) < 0)
    {
        printf("Unable to bind socket. Exiting.\n");
        exit(1);
    }

    if (listen(sockfd, BACKLOG) == -1)
    {
        printf("Unable to listen on socket. Exiting.\n");
        exit(1);
    }

    if (chdir("cached") == -1)
    {
        perror("cannot change to cached dir");
        exit(1);
    }

    // clear cached directory
    DIR *dir;
    struct dirent *ent;
    dir = opendir(".");
    if (!dir)
    {
        perror("cannot open cached dir");
        exit(1);
    }
    while ((ent = readdir(dir)) != NULL)
    {
        // ignore . and .. entries
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        // remove all others
        remove(ent->d_name);
    }

    // get new incoming connections
    int new_sockfd;
    int n = 0;
    while (new_sockfd = accept(sockfd, (struct sockaddr*)&remote, &remote_length))
    {
        printf("New connection received.\n");
        pthread_t thread;
        if (n == 10000)
            continue;
        if (pthread_create(&thread, NULL, handle_connection, (void*)&new_sockfd))
        {
            printf("Failed to create thread.\n");
        }
        n++;

        // remove timed out cache files
        clock_t curr_time = clock();
        if ((clock()-last_cleanup)/CLOCKS_PER_SEC>10)
        {
            for (int i = 0; i < CACHE_SIZE; i++)
            {
                if (occupied[i])
                {
                    if ((curr_time-entered_cache[i])/CLOCKS_PER_SEC>cache_timeout)
                    {
                        remove(hashes[i]);
                        occupied[i] = 0;
                    }
                }
            }
        }
    }

    close(sockfd);
}