#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define NUM_SERVERS 4
#define MAX_FILES 30
#define MAX_FILENAME 200
#define MAX_USER_CHARS 100
#define MAX_SERVER_CHARS 100
#define BUFFER_SIZE 3000
#define CONTROL_BYTES 250

char username[MAX_USER_CHARS];
char password[MAX_USER_CHARS];

char servers[NUM_SERVERS][3][MAX_SERVER_CHARS];
struct sockaddr_in server_sa[NUM_SERVERS];

char file_part_map[NUM_SERVERS][NUM_SERVERS][2] = {{{1,2},{2,3},{3,4},{4,1}},
                                                   {{4,1},{1,2},{2,3},{3,4}},
                                                   {{3,4},{4,1},{1,2},{2,3}},
                                                   {{2,3},{3,4},{4,1},{1,2}}};

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

int recv_timeout(int sockfd, int server_idx, char *buffer, int size)
{
    clock_t start = clock();
    while ((clock()-start)/CLOCKS_PER_SEC<=1)
    {
        int nbytes = recv(sockfd, buffer, size, MSG_DONTWAIT);
        if (nbytes > 0)
            return nbytes;
    }

    printf("Timeout occurred for server %s.\n", servers[server_idx][0]);

    return 0;
}

int error_present(int sockfd, int server_idx)
{
    char control_line[CONTROL_BYTES];
    int nbytes = recv_timeout(sockfd, server_idx, control_line, CONTROL_BYTES);
    if (!nbytes)
        return 1;

    char *saveptr;
    char *status = strtok_r(control_line, "$", &saveptr);

    if (strcmp(status, "error") == 0)
    {
        char *msg = strtok_r(0, "$", &saveptr);
        printf("Error encountered in server %s: %s\n", servers[server_idx][0], msg);
        return 1;
    }
    else if (strcmp(status, "done") == 0)
    {
        return 1;
    }

    return 0;
}

void send_authentication(int sockfd)
{
    char control_line[CONTROL_BYTES];
    strcpy(control_line, username);
    strcat(control_line, " ");
    strcat(control_line, password);
    send(sockfd, control_line, CONTROL_BYTES, 0);
}

void issue_list(char *subfolder)
{
    char buffer[BUFFER_SIZE];
    char control_line[CONTROL_BYTES];
    strcpy(control_line, "list");
    strcat(control_line, " ");
    strcat(control_line, subfolder);

    char files[MAX_FILES][MAX_FILENAME];
    char dirs[MAX_FILES][MAX_FILENAME];
    int parts_detected[MAX_FILES][NUM_SERVERS] = {0};
    int dirs_detected = 0;
    int files_detected = 0;

    for (int i = 0; i < NUM_SERVERS; i++)
    {
        // create socket associated with this server
        int sockfd = socket(PF_INET, SOCK_STREAM, 0);
        if (!sockfd)
        {
            printf("Failed to create socket associated with server %s.\n", servers[i][0]);
            continue;
        }

        // make actual connections
        socklen_t addrlen = sizeof(struct sockaddr);
        int rc = connect(sockfd, (struct sockaddr *)&server_sa[i], addrlen);
        if (rc == -1)
        {
            printf("Server %s failed to connect.\n", servers[i][0]);
            continue;
        }

        send_authentication(sockfd);
        int nbytes = send(sockfd, control_line, CONTROL_BYTES, 0);
        if (error_present(sockfd, i))
            continue;

        // assume all output fits in buffer
        recv_timeout(sockfd, i, buffer, BUFFER_SIZE-1);

        char *saveptr;
        char *file = strtok_r(buffer, "\n", &saveptr);
        while (file != NULL)
        {
            char *part_s = strrchr(file, '.');
            if (!part_s)
            {
                // this is a directory
                int already_detected = 0;
                for (int j = 0; j < dirs_detected; j++)
                {
                    if (strcmp(dirs[j], file) == 0)
                    {
                        already_detected = 1;
                    }
                }
                if (!already_detected)
                {
                    strcpy(dirs[dirs_detected], file);
                    dirs_detected++;
                }
            }
            else
            {
                // this is a regular file
                int part = strtol(part_s+1, 0, 10);
                *part_s = '\0';

                // determine if file is already in array
                int already_detected = 0;
                for (int j = 0; j < files_detected; j++)
                {
                    if (strcmp(files[j], file) == 0)
                    {
                        parts_detected[j][part-1] = 1;

                        already_detected = 1;
                    }
                }
                if (!already_detected)
                {
                    strcpy(files[files_detected], file);
                    parts_detected[files_detected][part-1] = 1;

                    files_detected++;
                }
            }

            file = strtok_r(0, "\n", &saveptr);
        }

        close(sockfd);  
    }

    // list detected dirs
    for (int i = 0; i < dirs_detected; i++)
    {
        printf("%s\n", dirs[i]);
    }

    // list detected files
    for (int i = 0; i < files_detected; i++)
    {
        int complete = 1;
        for (int j = 0; j < NUM_SERVERS; j++)
        {
            if (parts_detected[i][j] == 0)
                complete = 0;
        }

        // add 1 to ignore leading .
        if (complete)
        {
            printf("%s\n", files[i]+1);
        }
        else
        {
            printf("%s [incomplete]\n", files[i]+1);
        }
    }


}

void issue_put(char *file, char *subfolder)
{
    char command[strlen("md5sum ") + strlen(file) + 1];
    strcpy(command, "md5sum ");
    strcat(command, file);

    FILE *pp = popen(command, "r");
    if (!pp)
    {
        perror("Failed to run md5sum command");
        return;
    }

    char hex_string[32];
    size_t nbytes = fread(hex_string, 1, 32, pp);
    if (nbytes < 32)
    {
        printf("Failed to read results of md5sum command.\n");
        return;
    }

    // we consider all components of md5sum but reduce them to 32-bit representation using XOR
    // note: the following was referenced while computing the below 6 lines
    // https://stackoverflow.com/questions/11180028/converting-md5-result-into-an-integer-in-c
    int part1, part2, part3, part4;
    sscanf(&hex_string[0], "%x", &part1);
    sscanf(&hex_string[8], "%x", &part2);
    sscanf(&hex_string[16], "%x", &part3);
    sscanf(&hex_string[24], "%x", &part4);
    int hash = part1^part2^part3^part4;

    int hash_index = hash % NUM_SERVERS;
    if (hash_index < 0)
        hash_index += NUM_SERVERS;


    // split the file into four parts
    FILE *fp = fopen(file, "r");
    if (!fp)
    {
        printf("Failed to open file.\n");
        return;
    }
    fseek(fp, 0L, SEEK_END);
    long size = ftell(fp);
    rewind(fp);

    // use the following part size for first three pieces
    // if there are extra bytes at end (up to 3), then
    // last piece will have slightly extra
    int part_sizes = size / NUM_SERVERS;
    FILE *tmpfiles[NUM_SERVERS];
    long file_sizes[NUM_SERVERS] = {0};
    char buffer[BUFFER_SIZE];
    for (int i = 0; i < NUM_SERVERS; i++)
    {
        tmpfiles[i] = tmpfile();
        long num_to_read;
        if (i == NUM_SERVERS-1)
            num_to_read = BUFFER_SIZE-1;
        else
            num_to_read = min(BUFFER_SIZE-1, part_sizes*(i+1)-ftell(fp));
        while ((nbytes = fread(buffer, 1, num_to_read, fp)) > 0)
        {
            file_sizes[i] += nbytes;
            size_t nwritten = fwrite(buffer, 1, nbytes, tmpfiles[i]);
            
            if (i == NUM_SERVERS-1)
                num_to_read = BUFFER_SIZE-1;
            else
                num_to_read = min(BUFFER_SIZE-1, part_sizes*(i+1)-ftell(fp));
        }

        rewind(tmpfiles[i]);
    }

    // upload these temp files to servers
    char control_line[CONTROL_BYTES];
    for (int i = 0; i < NUM_SERVERS; i++)
    {
        // create socket associated with this server
        int sockfd = socket(PF_INET, SOCK_STREAM, 0);
        if (!sockfd)
        {
            printf("Failed to create socket associated with server %s.\n", servers[i][0]);
            return;
        }

        // make actual connections
        socklen_t addrlen = sizeof(struct sockaddr);
        int rc = connect(sockfd, (struct sockaddr *)&server_sa[i], addrlen);
        if (rc == -1)
        {
            perror("Failed to connect to server");
            printf("Server %s failed to connect. Will not PUT file when one or more servers is down.\n", servers[i][0]);
            return;
        }

        // send first two lines of control info
        send_authentication(sockfd);
        strcpy(control_line, "put ");
        strcat(control_line, file);
        strcat(control_line, " ");
        strcat(control_line, subfolder);
        send(sockfd, control_line, CONTROL_BYTES, 0);

        // send file parts
        for (int j = 0; j < 2; j++)
        {
            int file_part = file_part_map[hash_index][i][j];
            char file_part_s[10];
            sprintf(file_part_s, "%d", file_part);

            char file_size_s[10];
            sprintf(file_size_s, "%ld", file_sizes[file_part-1]);
            
            // start with file part number and then number of bytes
            strcpy(control_line, file_part_s);
            strcat(control_line, " ");
            strcat(control_line, file_size_s);

            // send this control information
            send(sockfd, control_line, CONTROL_BYTES, 0);

            // and then send file contents
            while ((nbytes = fread(buffer, 1, BUFFER_SIZE-1, tmpfiles[file_part-1])) > 0)
            {
                send(sockfd, buffer, nbytes, 0);
            }
            rewind(tmpfiles[file_part-1]);
        }

        if (error_present(sockfd, i))
        {
            printf("Cannot PUT when one or more servers fail.\n");
            return;
        }

        close(sockfd);
    }

    // close temp files
    for (int i = 0; i < NUM_SERVERS; i++)
    {
        fclose(tmpfiles[i]);
    }

    printf("Successfully uploaded file to servers.\n");
}

void issue_get(char *file, char *subfolder)
{
    // upload these temp files to servers
    char buffer[BUFFER_SIZE];
    char control_line[CONTROL_BYTES];
    int nbytes;
    int needed_parts[NUM_SERVERS];
    for (int i = 0; i < NUM_SERVERS; i++)
    {
        needed_parts[i] = 1;
    }
    FILE *tmpfiles[NUM_SERVERS];
    int has_all = 0;
    for (int i = 0; i < NUM_SERVERS; i++)
    {
        // create socket associated with this server
        int sockfd = socket(PF_INET, SOCK_STREAM, 0);
        if (!sockfd)
        {
            printf("Failed to create socket associated with server %s.\n", servers[i][0]);
            continue;
        }

        // make actual connections
        socklen_t addrlen = sizeof(struct sockaddr);
        int rc = connect(sockfd, (struct sockaddr *)&server_sa[i], addrlen);
        if (rc == -1)
        {
            perror("Failed to connect to server");
            continue;
        }

        // send first two lines of control info
        send_authentication(sockfd);
        strcpy(control_line, "get ");
        strcat(control_line, file);
        strcat(control_line, " ");
        strcat(control_line, subfolder);
        send(sockfd, control_line, CONTROL_BYTES, 0);

        // the subsequent line will include the required parts of the file for optimization purposes
        strcpy(control_line, "");
        for (int j = 0; j < NUM_SERVERS; j++)
        {
            if (needed_parts[j])
            {
                char part_needed[10];
                sprintf(part_needed, "%d ", j+1);
                strcat(control_line, part_needed);
            }
        }

        send(sockfd, control_line, CONTROL_BYTES, 0);

        // receive file parts
        for (int j = 0; j < 2; j++)
        {
            // all return info will be preceded by file part number and then # bytes
            if (error_present(sockfd, i))
                break;
            if (!recv_timeout(sockfd, i, control_line, CONTROL_BYTES))
                break;
            
            char *saveptr;
            char *part = strtok_r(control_line, " ", &saveptr);
            char *num_bytes_s = strtok_r(0, " ", &saveptr);
            int part_num = strtol(part, 0, 10);
            long num_bytes = strtol(num_bytes_s, 0, 10);

            // get file contents
            tmpfiles[part_num-1] = tmpfile();
            long num_read = 0;
            long num_to_read = min(BUFFER_SIZE-1, num_bytes-num_read);
            while (num_to_read != 0 && (nbytes = recv_timeout(sockfd, i, buffer, num_to_read)) > 0)
            {
                size_t nwritten = fwrite(buffer, 1, nbytes, tmpfiles[part_num-1]);
                if (nbytes != nwritten || nbytes != num_to_read)
                {
                    printf("Incorrect number of bytes read or written.\n");

                }
                num_read += nbytes;
                num_to_read = min(BUFFER_SIZE-1, num_bytes-num_read);
            }
            if (num_read != num_bytes)
            {
                printf("Did not read correct number of bytes.\n");
            }
            else
            {
                needed_parts[part_num-1] = 0;
            }
            rewind(tmpfiles[part_num-1]);
        }

        close(sockfd);

        // check if has all file parts
        has_all = 1;
        for (int j = 0; j < NUM_SERVERS; j++)
        {
            if (needed_parts[j] == 1)
                has_all = 0;
        }
        if (has_all)
            break;
    }

     // check if has all file parts
    if (!has_all)
    {
        printf("File is incomplete or does not exist.\n");
    }
    else
    {
        // construct file from parts
        FILE *fp = fopen(file, "w");
        for (int i = 0; i < NUM_SERVERS; i++)
        {
            while ((nbytes = fread(buffer, 1, BUFFER_SIZE-1, tmpfiles[i])) > 0)
            {
                fwrite(buffer, 1, nbytes, fp);
            }
        }
        fclose(fp);

        printf("Successfully retrieved file from servers.\n");

        // close temp files
        for (int i = 0; i < NUM_SERVERS; i++)
        {
            fclose(tmpfiles[i]);
        } 
    }
}

void issue_mkdir(char *dirname)
{
    char control_line[CONTROL_BYTES];
    for (int i = 0; i < NUM_SERVERS; i++)
    {
        // create socket associated with this server
        int sockfd = socket(PF_INET, SOCK_STREAM, 0);
        if (!sockfd)
        {
            printf("Failed to create socket associated with server %s.\n", servers[i][0]);
            return;
        }

        // make actual connections
        socklen_t addrlen = sizeof(struct sockaddr);
        int rc = connect(sockfd, (struct sockaddr *)&server_sa[i], addrlen);
        if (rc == -1)
        {
            perror("Failed to connect to server");
            printf("Server %s failed to connect. Will create directory when one or more servers is down.\n", servers[i][0]);
            return;
        }

        // send first two lines of control info
        send_authentication(sockfd);
        strcpy(control_line, "mkdir ");
        strcat(control_line, dirname);
        send(sockfd, control_line, CONTROL_BYTES, 0);

        if (error_present(sockfd, i))
        {
            printf("Cannot MKDIR when one or more servers fail.\n");
            return;
        }

        close(sockfd);
    }
}

int main(int argc, char **argv)
{
    bzero(username, MAX_USER_CHARS);
    bzero(password, MAX_USER_CHARS);

    if (argc != 2) {
        printf("Usage: dfc <conf_file>\n");
        exit(1);
    }

    char *conf_file = argv[1];

    // read conf file in order to obtain acceptable username/password combos
    int server_cnt = 0;
    char *line = 0;
    FILE *fp = fopen(conf_file, "r");
    if (!fp)
    {
        printf("Unable to open '%s'.\n", conf_file);
        exit(1);
    }

    ssize_t read;
    size_t len = 0;
    while ((read = getline(&line, &len, fp)) != -1)
    {
        char *saveptr;
        char *first_token = strtok_r(line, " :\n", &saveptr);
        tolower_str(first_token);
        if (strcmp(first_token, "server") == 0)
        {
            char *server_name = strtok_r(0, " :\n", &saveptr);
            char *ip_addr = strtok_r(0, " :\n", &saveptr);
            char *port_num = strtok_r(0, " :\n", &saveptr);

            if (!server_name || !ip_addr || !port_num ||
                strlen(server_name) >= MAX_SERVER_CHARS ||
                strlen(ip_addr) >= MAX_SERVER_CHARS ||
                strlen(port_num) >= MAX_SERVER_CHARS)
            {
                printf("Server line invalid.\n");
                exit(1);
            }

            // copy information in string format for later printing
            strcpy(servers[server_cnt][0], server_name);
            strcat(servers[server_cnt][1], ip_addr);
            strcat(servers[server_cnt][2], port_num);

            // convert to sockaddr for later use
            bzero(&server_sa[server_cnt], sizeof(server_sa[0]));
            server_sa[server_cnt].sin_family = AF_INET;
            server_sa[server_cnt].sin_port = htons(atoi(port_num));
            server_sa[server_cnt].sin_addr.s_addr = inet_addr(ip_addr); 

            server_cnt++;
        }
        else if (strcmp(first_token, "username") == 0)
        {
            char *conf_username = strtok_r(0, " :\n", &saveptr);
            if (strlen(conf_username) >= MAX_USER_CHARS)
            {
                printf("Username line invalid.\n");
                exit(1);
            }
            strcpy(username, conf_username);
        }
        else if (strcmp(first_token, "password") == 0)
        {
            char *conf_password = strtok_r(0, " :\n", &saveptr);
            if (!conf_password || strlen(conf_password) >= MAX_USER_CHARS)
            {
                printf("Password line invalid.\n");
                exit(1);
            }
            strcpy(password, conf_password);
        }
    }

    if (server_cnt != NUM_SERVERS)
    {
        printf("Only read %d servers from conf file. Expected %d.\n", server_cnt, NUM_SERVERS);
        exit(1);
    }

    if (strlen(username) == 0 || strlen(password) == 0)
    {
        printf("Authentication not fully specified. Username or password not read.\n");
        exit(1);
    }

    // display read users and servers
    printf("Read in the following servers and configuration from '%s':\n", conf_file);
    for (int i = 0; i < server_cnt; i++)
    {
        printf("Server Name: %s, IP Address: %s, Port Number: %s\n", servers[i][0], servers[i][1], servers[i][2]);
    }
    printf("Username: %s, Password: %s\n", username, password);

    // start reading in user inputs
    printf("\nPlease enter a command ('LIST', 'GET <file>', 'PUT <file>', or 'MKDIR <dir>'):\n");
    while ((read = getline(&line, &len, stdin)) != -1)
    {
        char *saveptr;
        char *first_token = strtok_r(line, " \n", &saveptr);
        if (!first_token)
        {
            printf("Please enter a command ('LIST', 'GET <file>', 'PUT <file>', or 'MKDIR <dir>'):\n");
            continue;
        }
        tolower_str(first_token);

        if (strcmp(first_token, "list") == 0)
        {
            char *subfolder = strtok_r(0, " \n", &saveptr);
            if (!subfolder)
            {
                subfolder = "./";
            }

            issue_list(subfolder);
        }
        else if (strcmp(first_token, "get") == 0)
        {
            char *file = strtok_r(0, " \n", &saveptr);
            char *subfolder = strtok_r(0, " \n", &saveptr);

            if (!file)
            {
                printf("Must specify file to GET.\n");
                printf("Please enter a command ('LIST', 'GET <file>', 'PUT <file>', or 'MKDIR <dir>'):\n");
                continue;
            }
            if (!subfolder)
            {
                subfolder = "./";
            }

            issue_get(file, subfolder);
        }
        else if (strcmp(first_token, "put") == 0)
        {
            char *file = strtok_r(0, " \n", &saveptr);
            char *subfolder = strtok_r(0, " \n", &saveptr);
            printf("[debug] %s : %s\n",file,subfolder);

            if (!file)
            {
                printf("Must specify file to PUT.\n");
                printf("Please enter a command ('LIST', 'GET <file>', 'PUT <file>', or 'MKDIR <dir>'):\n");
                continue;
            }
            if (!subfolder)
            {
                subfolder = "./";
            }

            issue_put(file, subfolder);
        }
        else if (strcmp(first_token, "mkdir") == 0)
        {
            char *dirname = strtok_r(0, " \n", &saveptr);
            if (!dirname)
            {
                printf("Must specify directory name.\n");
                printf("Please enter a command ('LIST', 'GET <file>', 'PUT <file>', or 'MKDIR <dir>'):\n");
                continue;
            }
            issue_mkdir(dirname);
        }
        else
        {
            printf("Command should be one of LIST, GET, PUT, or MKDIR.\n");
        }

        printf("Please enter a command ('LIST', 'GET <file>', 'PUT <file>', or 'MKDIR <dir>'):\n");
    }
    free(line);
}