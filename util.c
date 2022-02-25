#ifndef _REENTRANT
#define _REENTRANT
#endif

#include <stdio.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include "util.h"

#define BACKLOG 20
#define MSG_SIZE 2048
#define FILENAME_SIZE 1024
#define CONNECTION_BUF_SIZE 50

int server_fd;

/**********************************************
 * init
   - port is the number of the port you want the server to be
     started on
   - initializes the connection acception/handling system
   - YOU MUST CALL THIS EXACTLY ONCE (not once per thread,
     but exactly one time, in the main thread of your program)
     BEFORE USING ANY OF THE FUNCTIONS BELOW
   - if init encounters any errors, it will call exit().
************************************************/
void init(int port) {
  // Initialize server socket
  if((server_fd = socket(PF_INET, SOCK_STREAM, 0)) == -1){
    perror("Failed to initialize server");
    exit(1);
  }

  // Set socket option
  int enable = 1;
  if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) == -1){
    close(server_fd);
    perror("Can't set socket option");
    exit(1);
  }
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  // Bind socket
  if(bind(server_fd, (struct sockaddr*) &addr, sizeof(addr)) == -1){
    close(server_fd);
    perror("Bind error: this port is already taken on this machine");
    exit(1);
  }

  // Listen for connections
  if(listen(server_fd, BACKLOG) == -1){
    close(server_fd);
    perror("Failed to initialize server");
    exit(1);
  }
}

/**********************************************
 * accept_connection - takes no parameters
   - returns a file descriptor for further request processing.
     DO NOT use the file descriptor on your own -- use
     get_request() instead.
   - if the return value is negative, the request should be ignored.
***********************************************/
int accept_connection() {
  struct sockaddr_in client_addr;
  socklen_t addr_size = sizeof(client_addr);
  int client_fd;
  if((client_fd = accept(server_fd, (struct sockaddr*) &client_addr, &addr_size)) == -1){
    perror("Failed to accept connection");
    return -1;
  }
  return client_fd;
}

/**********************************************
 * get_request
   - parameters:
      - fd is the file descriptor obtained by accept_connection()
        from where you wish to get a request
      - filename is the location of a character buffer in which
        this function should store the requested filename. (Buffer
        should be of size 1024 bytes.)
   - returns 0 on success, nonzero on failure. You must account
     for failures because some connections might send faulty
     requests. This is a recoverable error - you must not exit
     inside the thread that called get_request. After an error, you
     must NOT use a return_request or return_error function for that
     specific 'connection'.
************************************************/
int get_request(int fd, char *filename, char *connection) {
  char msg[MSG_SIZE];
  int read_size = 0;
  memset(filename, '\0', MSG_SIZE);
  memset(connection, '\0', CONNECTION_BUF_SIZE);

  // Read message
  read_size = read(fd, msg, MSG_SIZE - 1);
  if(read_size > 0){
    msg[read_size] = '\0';
    int msgIndex = 0;
    int filenameIndex = 0;
    //printf("%s\n", msg);

    // Check for Get request
    char get[10];
    while(msg[msgIndex] != ' '){
      get[msgIndex] = msg[msgIndex];
      msgIndex++;
    }
    msgIndex++;
    if(strcmp(get, "GET") != 0){
      close(fd);
      perror("Failed to get request - not a GET request");
      return -1;
    }
    // Get filename
    while(msg[msgIndex] != ' '){
      // Check if filename is too big
      if(filenameIndex > FILENAME_SIZE - 1){
        close(fd);
        perror("Failed to get request - filename size too large");
        return -1;
      }
      // Check for .. and //
      if(msgIndex > 0){
        if((msg[msgIndex] == '.' && msg[msgIndex - 1] == '.') || 
           (msg[msgIndex] == '/' && msg[msgIndex - 1] == '/')){
             close(fd);
             perror("Failed to get request - bad filename");
             return -1;
        }
      }
      filename[filenameIndex] = msg[msgIndex];
      msgIndex++;
      filenameIndex++;
    }
    
    // Parse message to find connection type
    int newlineCounter = 0;
    int connectionIndex = 0;
    // Find connection header line
    while(newlineCounter < 5){
      if(msg[msgIndex] == '\n'){
        newlineCounter++;
      }
      msgIndex++;
    }
    // Find connection type
    msgIndex += 12;
    while(msg[msgIndex] != '\n'){
      connection[connectionIndex] = msg[msgIndex];
      msgIndex++;
      connectionIndex++;
    }
    connection[connectionIndex - 1] = '\0';
    //printf("Connection check: %s\n", connection);

    // Check for connection
    if((strcmp(connection, "Close") != 0) && (strcmp(connection, "Keep-Alive") != 0)){
      close(fd);
      perror("Failed to get request - wrong connection type");
      return -1;
    }

    // Check for filename
    if(strcmp(filename, "") == 0){
      close(fd);
      perror("Failed to get request - no filename");
      return -1;
    }
    return 0;
  }
  else if(read_size == 0){ // Nothing read
    //printf("read nothing\n");
    return -1;
  }
  else{ // Error
    close(fd);
    perror("Failed to get request - failed to read message");
    return -1;
  }
}

/**********************************************
 * return_result
   - returns the contents of a file to the requesting client
   - parameters:
      - fd is the file descriptor obtained by accept_connection()
        to where you wish to return the result of a request
      - content_type is a pointer to a string that indicates the
        type of content being returned. possible types include
        "text/html", "text/plain", "image/gif", "image/jpeg" cor-
        responding to .html, .txt, .gif, .jpg files.
      - buf is a pointer to a memory location where the requested
        file has been read into memory (the heap). return_result
        will use this memory location to return the result to the
        user. (remember to use -D_REENTRANT for CFLAGS.) you may
        safely deallocate the memory after the call to
        return_result (if it will not be cached).
      - numbytes is the number of bytes the file takes up in buf
   - returns 0 on success, nonzero on failure.
************************************************/
int return_result(int fd, char *content_type, char *buf, int numbytes, bool connection_persistent) {
  char header[MSG_SIZE];
  memset(header, '\0', MSG_SIZE);

  // Write out result from request with closed connection
  if(!connection_persistent){
    sprintf(header, "HTTP/1.1 200 OK\nContent-Type: %s\nContent-Length: %d\nConnection: Close\n\n",
            content_type, numbytes);

    // Write header
    if(write(fd, header, strlen(header)) != strlen(header)){
      close(fd);
      perror("Failed to send result");
      return -1;
    }
    // Write file contents in buf
    if(write(fd, buf, numbytes) != numbytes){
      close(fd);
      perror("Failed to send result");
      return -1;
    }
    close(fd);
    return 0;
  }
  // Write out result from request with persistent connection
  else{
    sprintf(header, "HTTP/1.1 200 OK\nContent-Type: %s\nContent-Length: %d\nConnection: Keep-Alive\n\n",
            content_type, numbytes);

    // Write header
    if(write(fd, header, strlen(header)) != strlen(header)){
      perror("Failed to send result");
      return -1;
    }
    // Write file contents in buf
    if(write(fd, buf, numbytes) != numbytes){
      perror("Failed to send result");
      return -1;
    }
    return 0;

  }
}

/**********************************************
 * return_error
   - returns an error message in response to a bad request
   - parameters:
      - fd is the file descriptor obtained by accept_connection()
        to where you wish to return the error
      - buf is a pointer to the location of the error text
   - returns 0 on success, nonzero on failure.
************************************************/
int return_error(int fd, char *buf, bool connection_persistent) {
  char header[MSG_SIZE];
  memset(header, '\0', MSG_SIZE);

  // Write out result from request with closed connection
  if(!connection_persistent){
    sprintf(header, "HTTP/1.1 404 Not Found\nContent-Type: text/html\nContent-Length: %ld\nConnection: Close\n\n",
            strlen(buf));

    // Write header
    if(write(fd, header, strlen(header)) != strlen(header)){
      close(fd);
      perror("Failed to send error");
      return -1;
    }
    // Write file contents in buf
    if(write(fd, buf, strlen(buf)) != strlen(buf)){
      close(fd);
      perror("Failed to send error");
      return -1;
    }
    close(fd);
    return 0;
  }
  // Write out result from request with persistent connection
  else{
    sprintf(header, "HTTP/1.1 404 Not Found\nContent-Type: text/html\nContent-Length: %ld\nConnection: Keep-Alive\n\n",
            strlen(buf));

    // Write header
    if(write(fd, header, strlen(header)) != strlen(header)){
      perror("Failed to send error");
      return -1;
    }
    // Write file contents in buf
    if(write(fd, buf, strlen(buf)) != strlen(buf)){
      perror("Failed to send error");
      return -1;
    }
    return 0;
  }
  
}
