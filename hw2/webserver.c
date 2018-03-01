#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
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
#define BUFFER_SIZE 1500


int sockfd;

int keep_alive_time;
char doc_root[MAX_CONF_LINE_SIZE];
char default_pages[MAX_DEFAULT_PAGES][MAX_CONF_LINE_SIZE];
char content_types[MAX_CONTENT_TYPES][2][MAX_CONF_LINE_SIZE];


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

void send_error(int new_sockfd, char *version, char *status, char *error_response, int keep_alive)
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
    if (keep_alive == 1)
        strcat(buffer, "keep-alive");
    else
        strcat(buffer, "close");
    strcat(buffer, "\n\n");
    strcat(buffer, error_response);
    send(new_sockfd, buffer, strlen(buffer), 0);

    free(error_response);
    free(status);
}

void send_400_error(int new_sockfd, char *version, char* reason, char *method, int keep_alive)
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
    send_error(new_sockfd, version, status, error_response, keep_alive);
}

void send_404_error(int new_sockfd, char *version, char *uri, int keep_alive)
{
    char *error_response = malloc(MAX_ERROR_SIZE);
    strcpy(error_response, "<html><body>404 Not Found. Reason: URL does not exist: ");
    strcat(error_response, uri);
    strcat(error_response, "</body></html>");
    char *status = malloc(MAX_STATUS_SIZE);
    strcpy(status, "404 Not Found");
    send_error(new_sockfd, version, status, error_response, keep_alive);
}

void send_501_error(int new_sockfd, char *version, char *method, int keep_alive)
{
    char *error_response = malloc(MAX_ERROR_SIZE);
    strcpy(error_response, "<html><body>501 Not Implemented. Reason: ");
    strcat(error_response, method);
    strcat(error_response, "</body></html>");
    char *status = malloc(MAX_STATUS_SIZE);
    strcpy(status, "501 Not Implemented");
    send_error(new_sockfd, version, status, error_response, keep_alive);
}

void send_500_error(int new_sockfd, char *version, char *error, int keep_alive)
{
    char *error_response = malloc(MAX_ERROR_SIZE);
    strcpy(error_response, "<html><body>500 Internal Server Error: ");
    strcat(error_response, error);
    strcat(error_response, "</body></html>");
    char *status = malloc(MAX_STATUS_SIZE);
    strcpy(status, "500 Internal Server Error");
    send_error(new_sockfd, version, status, error_response, keep_alive);
}

void send_file(int new_sockfd, char* method, char *uri, char *version, char *post_data, int keep_alive)
{
    char mime[MAX_CONF_LINE_SIZE];
    bzero(mime, MAX_CONF_LINE_SIZE);

    // get the mime type for the file
    char *ext = get_ext(uri);

    // POST request only acceptable on html files
    if (strcmp(method, "post") == 0 && strcmp(ext, "html") != 0)
    {
        send_501_error(new_sockfd, version, method, keep_alive);
        return;
    }

    // also make sure post request actually has allocated something
    if (strcmp(method, "post") == 0 && !post_data)
    {
        send_500_error(new_sockfd, version, "POST data has not been allocated.", keep_alive);
        return;
    }

    int content_cntr = 0;
    while (strcmp(content_types[content_cntr][0], "") != 0)
    {
        if (strcmp(content_types[content_cntr][0], ext) == 0)
        {
            strcpy(mime, content_types[content_cntr][1]);
            break;
        }
        content_cntr++;
    }
    if (strcmp(mime, "") == 0)
    {
        send_500_error(new_sockfd, version, "Extension not registered.", keep_alive);
        return;
    }

    // determine the file size of the requested file
    FILE *fp = fopen(uri, "r");
    if (!fp)
    {
        send_500_error(new_sockfd, version, "Could not open specified file.", keep_alive);
        return;
    }
    fseek(fp, 0L, SEEK_END);
    int size = ftell(fp);
    rewind(fp);

    // must increase size for post data if we are sending it
    if (post_data)
    {
        size += strlen(post_data) + strlen("<html><body><h1>POST DATA</h1><pre></pre></body></html>");
    }

    char size_str[200];
    sprintf(size_str, "%d", size);

    // actually send the headers/file
    char buffer[BUFFER_SIZE];
    bzero(buffer, BUFFER_SIZE);

    strcpy(buffer, "HTTP/");
    strcat(buffer, version);
    strcat(buffer, " 200 OK");
    strcat(buffer, "\nContent-Type: ");
    strcat(buffer, mime);
    strcat(buffer, "\nContent-Length: ");
    strcat(buffer, size_str);
    strcat(buffer, "\nConnection: ");
    if (keep_alive == 1)
        strcat(buffer, "keep-alive");
    else
        strcat(buffer, "close");

    strcat(buffer, "\n\n");

    if (post_data)
    {
        strcat(buffer, "<html><body><h1>POST DATA</h1><pre>");
        strcat(buffer, post_data);
        strcat(buffer, "</pre></body></html>");
    }
    send(new_sockfd, buffer, strlen(buffer), 0);

    int nbytes = 0;
    while (!feof(fp))
    {
        nbytes = fread(buffer, 1, BUFFER_SIZE, fp);
        if (nbytes > 0)
            send(new_sockfd, buffer, nbytes, 0);
    }
}


void handle_request(int new_sockfd, char *method, char *uri, char *version, char* post_data, int keep_alive)
{
    char error_response[MAX_ERROR_SIZE];

    // ensure proper version of HTTP is used
    if (strcmp(version, "1.1") != 0 && strcmp(version, "1.0") != 0)
    {
        send_400_error(new_sockfd, version, "version", version, keep_alive);
        return;
    }

    // make sure method is acceptable
    if (strcmp(method, "get") != 0 && strcmp(method, "post") != 0)
    {
        send_501_error(new_sockfd, version, method, keep_alive);
        return;
    }

    char fname[MAX_URI];
    strcpy(fname, doc_root);
    strcat(fname, uri);
    // if directory is specified, use default pages
    if (uri[strlen(uri)-1] == '/')
    {
        int base_length = strlen(fname);
        int page_cntr = 0;
        while (strcmp(default_pages[page_cntr], "") != 0)
        {
            strcpy(fname+base_length, default_pages[page_cntr]);
            if (access(fname, F_OK) != -1)
            {
                send_file(new_sockfd, method, fname, version, post_data, keep_alive);
                return;
            }
            page_cntr++;
        }
        // if this is reached, points to directory without default page
        send_404_error(new_sockfd, version, uri, keep_alive);
        return;
    }
    if (access(fname, F_OK) != -1)
    {
        // check that the requested file has a valid extension according to our webserver
        send_file(new_sockfd, method, fname, version, post_data, keep_alive);
    }
    else
    {
        // if this is reached, points to file that doesn't exist
        send_404_error(new_sockfd, version, uri, keep_alive);
        return;
    }
}

void *handle_connection(void *new_sockfd_p)
{
    int new_sockfd = *((int*)new_sockfd_p);

    int nbytes;
    char buffer[BUFFER_SIZE];
    char method[5];
    char uri[MAX_HEADER_LINE_SIZE];
    char version[4];
    char header_line[MAX_HEADER_LINE_SIZE];
    int content_size = -1;
    int keep_alive = 0;
    clock_t last_refresh = clock();
    char *post_data = 0;
    while ((clock()-last_refresh)/CLOCKS_PER_SEC <= keep_alive_time)
    {
        keep_alive = 0;
        while ((nbytes = recv(new_sockfd, buffer, BUFFER_SIZE-1, 0)) > 0)
        {
            // make buffer terminate with null char
            buffer[nbytes] = '\0';

            printf("-------------------------------------\nServing with sockfd: %d\n", new_sockfd);
            printf("%s-------------------------------------", buffer);

            // note: http connections apparently use \r\n instead of \n
            char *line = buffer;
            char *location = strchr(line, '\r');
            *location = '\0';
            sscanf(line, "%s %s HTTP/%s\r\n", method, uri, version);
            lower_string(method);
            line = location+2;
            while (location<buffer+BUFFER_SIZE && (location = strchr(line, '\r')))
            {
                *location = '\0';
                if (strcmp(line, "") == 0)
                {
                    // the headers have completed; read document body if told to
                    if (strcmp(method, "post") == 0)
                    {
                        if (content_size == -1)
                        {
                            send_400_error(new_sockfd, version, "method", "Content-Length not specified", keep_alive);
                        }
                        post_data = malloc(content_size+1);
                        strcpy(post_data, location+2);
                    }
                    handle_request(new_sockfd, method, uri, version, post_data, keep_alive);

                    // refresh clock if necessary
                    if (keep_alive == 1)
                    {
                        last_refresh = clock();
                    }
                }
                char *saveptr;
                char *term = strtok_r(line, " ", &saveptr);

                if (term)
                {
                    lower_string(term);
                    if (strcmp(term, "content-length:") == 0)
                    {
                        char *length = strtok_r(NULL, " ", &saveptr);
                        content_size = strtol(length, NULL, 10);
                    }
                    if (strcmp(term, "connection:") == 0)
                    {
                        char *type = strtok_r(NULL, " ", &saveptr);
                        if (type)
                        {
                            lower_string(type);
                            if (strcmp(type, "keep-alive") == 0)
                            {
                                keep_alive = 1;
                            }
                        }
                    }
                    line = location+2;
                }
            }
        }
        if (!keep_alive)
            break;
    }
    free(post_data);

    close(new_sockfd);
}

int main()
{
    // handle signals in order to properly close sockfds
    if (signal(SIGINT, sig_handler) == SIG_ERR)
        printf("Can't catch SIGINT.\n");
    if (signal(SIGTERM, sig_handler) == SIG_ERR)
        printf("Can't catch SIGTERM.\n");


    // read configuration file
    FILE *fp;
    char buff[MAX_CONF_LINE_SIZE];

    fp = fopen("ws.conf", "r");
    int port;

    // zero out buffers so we can use their nullness if not yet assigned
    bzero(doc_root, sizeof(MAX_CONF_LINE_SIZE));
    bzero(default_pages, sizeof(char[MAX_DEFAULT_PAGES][MAX_CONF_LINE_SIZE]));
    bzero(content_types, sizeof(char[MAX_CONTENT_TYPES][2][MAX_CONF_LINE_SIZE]));

    int content_cnt = 0;
    while (fgets(buff, MAX_CONF_LINE_SIZE, fp))
    {
        remove_newline(buff);

        // if line starts with ".", assume it gives content type mapping
        if (buff[0] == '.')
        {
            char *saveptr;
            char *first = strtok_r(buff, " ", &saveptr);
            char *second = strtok_r(NULL, " ", &saveptr);
            if (first && second)
            {
                strcpy(content_types[content_cnt][0], first+1);
                strcpy(content_types[content_cnt][1], second);
                content_cnt++;
            }
        }
        // otherwise look for a directive to process
        else
        {
            char *saveptr;
            char *directive = strtok_r(buff, " ", &saveptr);
            if (directive)
            {
                if (strcmp(directive, "Listen") == 0)
                {
                    char *port_str = strtok_r(NULL, " ", &saveptr);
                    port = strtol(port_str, NULL, 10);
                }
                else if (strcmp(buff, "DocumentRoot") == 0)
                {
                    char *doc_root_str = strtok_r(NULL, " ", &saveptr);
                    // take out quotes if present
                    if (*doc_root_str == '"')
                        doc_root_str++;
                    if (doc_root_str[strlen(doc_root_str)-1] == '"')
                        doc_root_str[strlen(doc_root_str)-1] = '\0';
                    strcpy(doc_root, doc_root_str);
                }
                else if (strcmp(buff, "DirectoryIndex") == 0)
                {
                    int i = 0;
                    char *split;
                    while (split = strtok_r(NULL, " ", &saveptr))
                    {
                        strcpy(default_pages[i], split);
                        i++;
                    }
                }
                else if (strcmp(buff, "Keep-Alive") == 0)
                {
                    char *time_str = strtok_r(NULL, " ", &saveptr);
                    keep_alive_time = strtol(time_str, NULL, 10);
                }
            }
        }
    }

    // print out all the settings that we have collected
    printf("The following settings have been read from configuration file:\n");
    printf("Port: %d\n", port);
    printf("Document Root: %s\n", doc_root);
    printf("Default Page(s):\n");
    int page_cntr = 0;
    while (strcmp(default_pages[page_cntr], "") != 0)
    {
        printf("- %s\n", default_pages[page_cntr]);
        page_cntr++;
    }
    printf("Content Mappings:\n");
    int content_cntr = 0;
    while (strcmp(content_types[content_cntr][0], "") != 0)
    {
        printf("%s -> %s\n", content_types[content_cntr][0], content_types[content_cntr][1]);
        content_cntr++;
    }
    printf("Keep-Alive: %d\n", keep_alive_time);

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

    listen(sockfd, BACKLOG);

    // get new incoming connections
    int new_sockfd;
    while (new_sockfd = accept(sockfd, (struct sockaddr*)&remote, &remote_length))
    {
        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_connection, (void*)&new_sockfd))
        {
            printf("Failed to create thread.\n");
        }
    }

    close(sockfd);
}