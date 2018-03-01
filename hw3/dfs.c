#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_USERS 30
#define MAX_USER_CHARS 100
#define BACKLOG 20
#define BUFFER_SIZE 3000
#define CONTROL_BYTES 250
#define NUM_SERVERS 4

char users[MAX_USERS][2][MAX_USER_CHARS];
int user_cnt = 0;

void tolower_str(char *str)
{
    for(int i = 0; str[i]; i++)
        str[i] = tolower(str[i]);
}

long min(long a, long b)
{
    if (a < b)
        return a;
    return b;
} 

void send_error(int connfd, char *error)
{
    char control_line[CONTROL_BYTES];
    strcpy(control_line, "error$");
    strcat(control_line, error);
    send(connfd, control_line, CONTROL_BYTES, 0);
    close(connfd);
    pthread_exit(0);
}

void send_success(int connfd)
{
    char control_line[CONTROL_BYTES];
    strcpy(control_line, "success$");
    send(connfd, control_line, CONTROL_BYTES, 0);
}

void send_done(int connfd)
{
    char control_line[CONTROL_BYTES];
    strcpy(control_line, "done$");
    send(connfd, control_line, CONTROL_BYTES, 0);
}


void receive_list(int connfd, char *username, char *subfolder)
{
    printf("Received list command from user %s.\n", username);

    char control_line[CONTROL_BYTES];

    char dirname[500];
    strcpy(dirname, username);
    strcat(dirname, "/");
    strcat(dirname, subfolder);

    // open directory for user and then read list
    DIR *dir;
    struct dirent *ent;
    dir = opendir(dirname);
    if (!dir)
    {
        send_error(connfd, strerror(errno));
    }

    // it is assumed that directory contents will not be larger than BUFFER_SIZE
    char response[BUFFER_SIZE];
    bzero(response, BUFFER_SIZE);
    while ((ent = readdir(dir)) != NULL)
    {
        // ignore . and .. entries
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        strcat(response, ent->d_name);
        strcat(response, "\n");
    }

    if (strlen(response) == 0)
    {
        send_error(connfd, "Directory empty.");
    }

    // return success result and then actual result
    send_success(connfd);
    send(connfd, response, strlen(response)+1, 0);
}

void receive_put(int connfd, char *username, char *file, char *subfolder)
{
    printf("Received put command from user %s.\n", username);

    char control_line[CONTROL_BYTES];
    char buffer[BUFFER_SIZE];

    for (int i = 0; i < 2; i++)
    {
        // read control line to determine part of file and num bytes
        int nbytes = recv(connfd, control_line, CONTROL_BYTES, 0);

        char *saveptr;
        char *part = strtok_r(control_line, " ", &saveptr);
        char *num_bytes_s = strtok_r(0, " ", &saveptr);
        long num_bytes = strtol(num_bytes_s, 0, 10);


        // we will write all read information to file
        char filename[125];
        strcpy(filename, username);
        strcat(filename, "/");
        strcat(filename, subfolder);
        strcat(filename, ".");
        strcat(filename, file);
        strcat(filename, ".");
        strcat(filename, part);
        FILE *fp = fopen(filename, "w");

        long num_read = 0;
        long num_to_read = min(BUFFER_SIZE-1, num_bytes-num_read);
        while (num_to_read != 0 && (nbytes = recv(connfd, buffer, num_to_read, 0)) > 0)
        {
            size_t nwritten = fwrite(buffer, 1, nbytes, fp);
            if (nbytes != nwritten || nbytes != num_to_read)
            {
                send_error(connfd, "Incorrect number of bytes read or written.");
            }
            num_read += nbytes;
            num_to_read = min(BUFFER_SIZE-1, num_bytes-num_read);
        }
        if (num_read != num_bytes)
        {
            send_error(connfd, "Did not read correct number of bytes.");
        }

        fclose(fp);
    }

    send_success(connfd);
}

void receive_mkdir(int connfd, char *username, char *dirname)
{
    printf("Received mkdir command from user %s.\n", username);

    char dir_full[125];
    strcpy(dir_full, username);
    strcat(dir_full, "/");
    strcat(dir_full, dirname);

    DIR *dir = opendir(dir_full);
    if (!dir && ENOENT == errno)
    {
        int rc = mkdir(dir_full, 0777);
        if (rc != 0)
            send_error(connfd, "Could not create directory.");
    }
    else if (!dir)
    {
        // failed for some other reason
        send_error(connfd, "Could not create directory.");
    }
    else
    {
        closedir(dir);
    }

    send_success(connfd);
}

void receive_get(int connfd, char *username, char *file, char *subfolder)
{
    printf("Received get command from user %s.\n", username);

    char control_line[CONTROL_BYTES];
    char buffer[BUFFER_SIZE];

    // read control line to determine part of file and num bytes
    int nbytes = recv(connfd, control_line, CONTROL_BYTES, 0);

    char needed_parts[NUM_SERVERS][10];
    char needed_idx = 0;

    char *saveptr;
    char *part = strtok_r(control_line, " ", &saveptr);
    while (part != NULL)
    {
        strcpy(needed_parts[needed_idx], part);
        needed_idx++;
        part = strtok_r(0, " ", &saveptr);
    }

    // now we try to open files with the given filename and part number
    for (int i = 0; i < needed_idx; i++)
    {
        char filename[125];
        strcpy(filename, username);
        strcat(filename, "/");
        strcat(filename, subfolder);
        strcat(filename, ".");
        strcat(filename, file);
        strcat(filename, ".");
        strcat(filename, needed_parts[i]);
        FILE *fp = fopen(filename, "r");
        // if file doesn't exist, just move on
        if (!fp)
            continue;

        // determine file size of part
        fseek(fp, 0L, SEEK_END);
        long size = ftell(fp);
        rewind(fp);
        char size_s[10];
        sprintf(size_s, "%ld", size);

        send_success(connfd);

        // give part number and number of bytes
        strcpy(control_line, needed_parts[i]);
        strcat(control_line, " ");
        strcat(control_line, size_s);

        send(connfd, control_line, CONTROL_BYTES, 0);


        while ((nbytes = fread(buffer, 1, BUFFER_SIZE-1, fp)) > 0)
        {
            send(connfd, buffer, nbytes, 0);
        }
    }

    // tell user that sent all parts it has
    send_done(connfd);
}

void *handle_connection(void *connfd_p)
{
    int connfd = *((int*)connfd_p);

    int nbytes;
    char control_line[CONTROL_BYTES];
    
    // read first control line to authenaticate uesr
    nbytes = recv(connfd, control_line, CONTROL_BYTES, 0);

    // first line will be username and password
    // we verify that these are correct
    char *saveptr;
    char *username = strtok_r(control_line, " \n", &saveptr);
    char *password = strtok_r(0, " \n", &saveptr);

    // save the username for later; we'll need it
    char username_saved[MAX_USER_CHARS];
    strcpy(username_saved, username);

    if (!username_saved || !password)
    {
        send_error(connfd, "Username or password not specified.");
    }

    int authenticated = 0;
    for (int i = 0; i < user_cnt; i++)
    {
        if (strcmp(username_saved, users[i][0]) == 0 &&
            strcmp(password, users[i][1]) == 0)
        {
            authenticated = 1;
            break;
        }
    }

    if (!authenticated)
    {
        send_error(connfd, "Incorrect username/password combo.");
    }

    // verify that the authenticated user has a directory
    // if not, create one for them
    DIR *dir = opendir(username_saved);
    if (!dir && ENOENT == errno)
    {
        int rc = mkdir(username_saved, 0777);
        if (rc != 0)
            send_error(connfd, "Could not create directory.");
    }
    else if (!dir)
    {
        // failed for some other reason
        send_error(connfd, "Could not open directory.");
    }
    else
    {
        closedir(dir);
    }

    // the second line will have the command issued
    nbytes = recv(connfd, control_line, CONTROL_BYTES, 0);

    char *command = strtok_r(control_line, " \n", &saveptr);
    if (!command)
    {
        send_error(connfd, "Could not read command.");
    }
    tolower_str(command);
    if (strcmp(command, "list") == 0)
    {
        char *subfolder = strtok_r(0, " \n", &saveptr);
        receive_list(connfd, username_saved, subfolder);
    }
    else if (strcmp(command, "get") == 0)
    {
        char *file = strtok_r(0, " \n", &saveptr);
        char *subfolder = strtok_r(0, " \n", &saveptr);

        if (!file || !subfolder)
        {
            send_error(connfd, "File path not specified.");
        }

        receive_get(connfd, username_saved, file, subfolder);
    }
    else if (strcmp(command, "put") == 0)
    {
        char *file = strtok_r(0, " \n", &saveptr);
        char *subfolder = strtok_r(0, " \n", &saveptr);
        if (!file || !subfolder)
        {
            send_error(connfd, "File path not specified.");
        }
        receive_put(connfd, username_saved, file, subfolder);
    }
    else if (strcmp(command, "mkdir") == 0)
    {
        char *dirname = strtok_r(0, " \n", &saveptr);
        if (!dirname)
        {
            send_error(connfd, "Directory not specified.");
        }
        receive_mkdir(connfd, username_saved, dirname);
    }

    close(connfd);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: dfs <directory> <port number>\n");
        exit(1);
    }

    char *directory = argv[1];
    char *port = argv[2];

    // read dfs.conf file in order to obtain acceptable username/password combos
    char *line = 0;
    FILE *fp = fopen("dfs.conf", "r");
    if (!fp)
    {
        printf("Unable to open 'dfs.conf'.\n");
        exit(1);
    }

    ssize_t read;
    size_t len = 0;
    while ((read = getline(&line, &len, fp)) != -1)
    {
        char *saveptr;
        char *username = strtok_r(line, " \n", &saveptr);
        char *password = strtok_r(0, " \n", &saveptr);

        if (username && strlen(username) < MAX_USER_CHARS &&
            password && strlen(password) < MAX_USER_CHARS)
        {
            strcpy(users[user_cnt][0], username);
            strcpy(users[user_cnt][1], password);
            user_cnt++;
        }
    }
    free(line);

     // create specified directory if it does not already exist
    DIR *dir = opendir(directory);
    if (!dir && ENOENT == errno)
    {
        int rc = mkdir(directory, 644);
        if (rc != 0)
        {
            perror("Couldn't create directory");
        }
    }
    else if (!dir)
    {
        // failed for some other reason
        perror("Couldn't open directory");
        exit(1);
    }
    else
    {
        closedir(dir);   
    }
    if (chdir(directory) == -1)
    {
        perror("Couldn't change directory");
        exit(1);
    }

    // display read users
    printf("Read in the following users from 'dfs.conf':\n");
    for (int i = 0; i < user_cnt; i++)
    {
        printf("Username: %s, Password: %s\n", users[i][0], users[i][1]);
    }

    // create socket, bind, and listen
    int listenfd;
    struct sockaddr_in sin, remote;
    unsigned int remote_length;

    bzero(&sin, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(strtol(port, 0, 10));
    sin.sin_addr.s_addr = INADDR_ANY;

    if ((listenfd = socket(PF_INET, SOCK_STREAM, 0)) < 0)
    {
        printf("Unable to create socket. Exiting.\n");
        exit(1);
    }

    if (bind(listenfd, (struct sockaddr*)&sin, sizeof(sin)) < 0)
    {
        printf("Unable to bind socket. Exiting.\n");
        exit(1);
    }

    listen(listenfd, BACKLOG);

    // get new incoming connections
    int connfd;
    while (connfd = accept(listenfd, (struct sockaddr*)&remote, &remote_length))
    {
        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_connection, (void*)&connfd))
        {
            printf("Failed to create thread.\n");
        }
    }
}