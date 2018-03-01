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
#define MAX_HEADER_LINE_SIZE 500
#define MAX_STATUS_SIZE 50
#define MAX_URI 400
#define MAX_ERROR_SIZE 200
#define MAX_DEFAULT_PAGES 10
#define MAX_CONTENT_TYPES 20
#define BACKLOG 100
#define BUFFER_SIZE 5000
#define CACHE_SIZE 200

int sockfd;

char blocked_ips[20][100];
char blocked_ip_idx = 0;
char blocked_hosts[20][100];
char blocked_host_idx = 0;

char hashes[CACHE_SIZE][33];
time_t entered_cache[CACHE_SIZE];
int occupied[CACHE_SIZE];

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

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
        return;
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

void send_cache(int new_sockfd, char *hash)
{
    FILE *fp = fopen(hash, "r");
    if (!fp)
    {
        send_500_error(new_sockfd, "1.1", "Could not open specified file.");
        return;
    }
    
    int nbytes = 0;
    char buffer[BUFFER_SIZE];
    while (!feof(fp))
    {
        nbytes = fread(buffer, 1, BUFFER_SIZE, fp);
        if (nbytes > 0)
            send(new_sockfd, buffer, nbytes, 0);
    }
    fclose(fp);
}

void exit_thread(int new_sockfd)
{
    printf("closing socket\n");
    close(new_sockfd);

    pthread_exit(0);
}

void *handle_connection(void *new_sockfd_p)
{
    int new_sockfd = *((int*)new_sockfd_p);

    char headers[BUFFER_SIZE*3];
    headers[0] = '\0';
    char buffer[BUFFER_SIZE];
    char method[5];
    char version[4];
    char uri[MAX_HEADER_LINE_SIZE];
    char header_line[MAX_HEADER_LINE_SIZE];
    int content_size = -1;

    printf("-------------------------------------\nServing with sockfd: %d\n", new_sockfd);

    int nbytes;
    size_t len;
    char *line;
    ssize_t read = 0;
    int headers_done = 0;
    int closed_conn = 0;
    while (!headers_done && (nbytes = recv(new_sockfd, buffer, BUFFER_SIZE-1, 0)) > 0)
    {
        buffer[nbytes] = '\0';

        FILE *fmem = fmemopen(buffer, strlen(buffer), "r");
        if (!fmem)
        {
            perror("could not open buffer memfile");
            exit_thread(new_sockfd);
        }
        while ((read = getline(&line, &len, fmem)) != -1)
        {
            if (strcmp(line, "\n") == 0 || strcmp(line, "\r\n") == 0)
            {
                headers_done = 1;
                if (!closed_conn)
                {
                    strcat(headers, "Connection: close\r\n");
                }
                strcat(headers, line);
                break;
            }

            char *cmd_end = strchr(line, ':');
            char *cmd;
            if (cmd_end)
            {
                cmd = malloc(cmd_end-line+1);
                memcpy(cmd, line, cmd_end-line+1);
                cmd[cmd_end-line] = '\0';
            }
            if (cmd)
            {
                lower_string(cmd);
                if (strcmp(cmd, "connection") == 0)
                {
                    strcat(headers, "Connection: close\r\n");
                    closed_conn = 1;
                    continue;
                }
                else if (strcmp(cmd, "if-modified-since") == 0
                    || strcmp(cmd, "proxy-connection") == 0)
                {
                    continue;
                }
            }
            strcat(headers, line);
        }
        fclose(fmem);
    }
    if (!headers_done)
    {
        printf("Failed to read received message\n");
        send_500_error(new_sockfd, "1.1", "Failed to read message.");
        exit_thread(new_sockfd);
    }

    printf("%s", headers);

    // note: http connections apparently use \r\n instead of \n
    char first_line[500];

    char *eol = strchr(headers, '\r');
    *eol = '\0';
    strcpy(first_line, headers);
    *eol = '\r';

    sscanf(first_line, "%s %s HTTP/%s\r\n", method, uri, version);

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
            exit_thread(new_sockfd);
        }
    }

    // ensure proper version of HTTP is used
    if (strcmp(version, "1.1") != 0 && strcmp(version, "1.0") != 0)
    {
        send_400_error(new_sockfd, version, "version", version);
        exit_thread(new_sockfd);
    }

    // make sure method is acceptable
    if (strcmp(method, "get") != 0)
    {
        send_501_error(new_sockfd, version, method);
        exit_thread(new_sockfd);
    }

    // compute md5sum hash string
    char command[strlen("echo '") + strlen(uri) + strlen("' | md5sum") + 1];
    strcpy(command, "echo '");
    strcat(command, uri);
    strcat(command, "' | md5sum");

    FILE *pp = popen(command, "r");
    if (!pp)
    {
        perror("Failed to run md5sum command");
        exit_thread(new_sockfd);
    }

    char hex_string[33];
    nbytes = fread(hex_string, 1, 32, pp);
    if (nbytes < 32)
    {
        printf("Failed to read results of md5sum command.\n");
        exit_thread(new_sockfd);
    }
    hex_string[32] = '\0';

    int use_cached = 0;
    if (access(hex_string,F_OK) != -1)
    {
        time_t curr_time;
        time(&curr_time);

        pthread_mutex_lock(&mutex);
        for (int i = 0; i < CACHE_SIZE; i++)
        {
            if (occupied[i] && strcmp(hashes[i], hex_string) == 0 &&
                difftime(curr_time, entered_cache[i]) <= cache_timeout)
            {
                use_cached = 1;
                break;
            }
        }
        pthread_mutex_unlock(&mutex);
    }

    if (use_cached)
    {
        printf("****************sending file from cache*******************\n");
        send_cache(new_sockfd, hex_string);
    }
    else
    {
        printf("******************not sending file from cache********************\n");

        struct sockaddr_in remote_addr;

        // check stored hosts for host
        int had_ip = check_cachedaddr(host, &remote_addr);
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
                exit_thread(new_sockfd);
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
            exit_thread(new_sockfd);
        }
        printf("presentation IP address: %s\n", ip_str);
        for (int i = 0; i < blocked_ip_idx; i++)
        {
            if (strcmp(blocked_ips[i], ip_str) == 0)
            {
                send_403_error(new_sockfd, version, uri);
                exit_thread(new_sockfd);
            }
        }

        // create socket and connect
        int remote_sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (remote_sockfd == -1)
        {
            perror("creation of remote socket failed");
            exit_thread(new_sockfd);
        }
        socklen_t addrlen = sizeof(struct sockaddr);
        if (connect(remote_sockfd, (struct sockaddr*)&remote_addr, addrlen) != 0)
        {
            printf("%d", errno);
            perror("connect call failed");
            exit_thread(new_sockfd);
        }

        // send this server the GET line
        printf("Sending original request to remote server:\n%s", headers);
        if (send(remote_sockfd, headers, strlen(headers), 0) == -1)
        {
            printf("relay of HTTP GET failed: %s\n", strerror(errno));
            exit_thread(new_sockfd);
        }

        // add file to cache at same time as sending back to client
        FILE *fcache = fopen(hex_string, "w");
        if (!fcache)
        {
            perror("Failed to open cache write file");
            exit_thread(new_sockfd);
        }

        printf("Created connection to %s. Will now echo received data to client.\n", uri);
        while ((nbytes = recv(remote_sockfd, buffer, BUFFER_SIZE-1, 0)) > 0)
        {
            // make buffer terminate with null char
            buffer[nbytes] = '\0';

            int wrote = fwrite(buffer, 1, nbytes, fcache);
            if (wrote != nbytes)
            {
                printf("error writing to cache file\n");
                exit_thread(new_sockfd);
            }
        }
        fclose(fcache);

        // now send cached file
        pthread_mutex_lock(&mutex);
        for (int i = 0; i < CACHE_SIZE; i++)
        {
            if (!occupied[i])
            {
                occupied[i] = 1;
                strcpy(hashes[i], hex_string);
                time(&entered_cache[i]);
                break;
            }
        }
        pthread_mutex_unlock(&mutex);
        send_cache(new_sockfd, hex_string);
    }

    exit_thread(new_sockfd);
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
        if (line[read-1] == '\n')
            line[read-1] = '\0';

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
    fclose(fp);

    printf("Blocked hosts:\n");
    for (int i = 0; i < blocked_host_idx; i++)
    {
        printf("- %s\n", blocked_hosts[i]);
    }
    printf("Blocked IPs:\n");
    for (int i = 0; i < blocked_ip_idx; i++)
    {
        printf("- %s\n", blocked_ips[i]);
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

    bzero(&sin, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = INADDR_ANY;

    if ((sockfd = socket(PF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("could not create socket");
        exit(1);
    }

    if (bind(sockfd, (struct sockaddr*)&sin, sizeof(sin)) == -1)
    {
        perror("cannot bind socket");
        exit(1);
    }

    if (listen(sockfd, BACKLOG) == -1)
    {
        perror("could not listen on socket");
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
    time_t last_cleanup;
    time(&last_cleanup);
    socklen_t remote_length = sizeof(struct sockaddr);
    while ((new_sockfd = accept(sockfd, (struct sockaddr*)&remote, &remote_length)) != -1)
    {
        printf("New connection received.\n");
        pthread_t thread;
        int err = pthread_create(&thread, NULL, handle_connection, (void*)&new_sockfd);
        if (err)
        {
            printf("failed to create thread: %s\n", strerror(err));
        }

        // remove timed out cache files
        time_t curr_time;
        time(&curr_time);
        if (difftime(curr_time, last_cleanup) > 1)
        {
            printf("******************cleaning cache********************\n");
            pthread_mutex_lock(&mutex);
            for (int i = 0; i < CACHE_SIZE; i++)
            {
                if (occupied[i])
                {
                    if (difftime(curr_time, entered_cache[i]) > cache_timeout)
                    {
                        printf("Removing %s\n", hashes[i]);
                        remove(hashes[i]);
                        occupied[i] = 0;
                    }
                }
            }
            pthread_mutex_unlock(&mutex);

            last_cleanup = curr_time;
        }
    }

    printf("errno: %d\n", errno);
    perror("cannot accept socket");
    exit(1);
    close(sockfd);
}