Jeff Everett
CSCI 4273
9/24/2017

Programming Assignment 1 Description

Use:
Both the client and server can be compiled with the command-line call "make"
in the appropriate directory.

Server: ./server <port>
Client: ./client <ip_addr> <port>

Example use:
./server 8459 (in directory serverFolder)
./client 128.138.201.66 8459 (in directory clientFolder)


Message Format:
The messages sent are limited to 512 bytes (given by MAXBUFSIZE in each file).
The first four of those bytes are reserved for a sequence number, and the fifth
byte is reserved for a flag indicating that the message is the last one that
will be sent for the current file transfer. The remainder holds the actual data.


Reliability:
The system has a reliability method for message transfers. It works as follows:
1.) Both the client and server store an incoming sequence number and an outgoing
sequence number.
2.) When the client and server send a message, they place the outgoing sequence
number in the first four bytes of the message and then increment the outgoing
sequence number.
3.) Every message is sent three times to ensure redundancy in case of lost
messages.
4.) When a message of the expected sequence number is received, the incoming
sequence number is incremented and then the message is processed.
5.) When a message is received with a sequence number below that which was
expected, it is discarded.
6.) When a message is received with a sequence number above that which was
expected, it is stored in a global buffer (which has the capacity to store
10,000 such messages).
7.) Before beginning the wait for a message with a new sequence number, this
global buffer is checked. If the global buffer contains a message with the
desired sequence number, it is returned from there and then the incoming
sequence number is incremented.
8.) There is a forced delay of 100,000 nanoseconds before sending each message.
This is to allow the server to catch up and not lose any messages by overflowing
its queue. The result is quite slow file transfer.

It is still possible for the system to be unsuccessful if, for example, all three
copies of a message are lost.


Restrictions:
There are a number of restrictions that must be enforced for proper operation
of the client and server:
1.) All non-file transfer operations must be completed in one message.
    Large filenames or directories with many files will violate this restriction.
2.) Server only has one client.
3.) If the server is restarted, the client is as well.
4.) If the client is restarted, the server is as well.
5.) Server is started before any commands are sent to it.
Restriction 1 is imposed for ease of programming, and Restrictions 2-5 are
imposed to ensure the sequence numbers of the client and server are synchronized. 


Functionality:
"exit" command - When the client is given the exit command,it sends an "exit"
    command to the remote server, instructing it to shut down. Then the client
    itself shuts down.
"ls" command - The client forwards the "ls" command to the remote server, which
    returns its local directory listing. The clients prints this information
    on the command line.
"delete" command - The client forwards the "delete" command to the remote server,
    which deletes the file from its local directory if it exists.
"put" command - The client sends one message to tell the remote server that it
    will be transferring a file. Then it sends however many messages are needed
    to actually transfer the file. The client communicates its last message in
    the file transfer by setting the fifth byte of the datagram to all 1s.
"get" command - The client sends one message to tell the remote server to
    transfer the requested file. The server then responds with however many
    messages are needed to transfer that file. The remote server communicates
    its last message in the file transfer by setting the fifth byte of the
    datagram to all 1s.



