#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include "util.h"
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h> // *ADDED*
#include <netinet/in.h> // *ADDED*

#define MAX_THREADS 100
#define MAX_queue_len 100
#define MAX_CE 100
#define INVALID -1
#define BUFF_SIZE 1024
#define CONNECTION_BUFF_SIZE 50

// structs:
typedef struct request_queue {
  int fd;
  char *request;
  bool connection_persistent; // *ADDED*
} request_t;

typedef struct cache_entry {
  int len;
  char *request;
  char *content;
} cache_entry_t;


// globals:
FILE *logfile;

// Dispatcher Threads
pthread_t dispatch_threads[MAX_THREADS];
// Worker Threads
pthread_t worker_threads[MAX_THREADS];
// Thread ID array
int wid_arr[MAX_THREADS];
int did_arr[MAX_THREADS];


pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t req_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t req_queue_notfull = PTHREAD_COND_INITIALIZER; // wait until queue isn't full (dispatch)
pthread_cond_t req_queue_notempty = PTHREAD_COND_INITIALIZER; // wait until queue isn't empty (worker)
request_t requests[MAX_queue_len];

int req_remove_index = 0;
int req_insert_index = 0;
int curr_queue_len = 0;
int queue_len;
int cache_size = INVALID;
int cache_evict_index = 0;
int num_worker = INVALID;

// Cache
cache_entry_t *cache;

void *dispatch(void *arg);

void *worker(void *arg);

// For grace termination
static volatile sig_atomic_t doneflag = 0;
static void setdoneflag(int signo) {
  doneflag = 1;
}

/* ************************ Dynamic Pool Code ***********************************/
bool alive_pool[MAX_THREADS];

// Extra Credit: This function implements the policy to change the worker thread pool dynamically
// depending on the number of requests
void *dynamic_pool_size_update(void *arg) {
  while (1) {
    usleep(500000);
    // Lock the mutex to dynamically change thread pool
    if (pthread_mutex_lock(&req_queue_mutex) < 0) {
      printf("Failed to lock queue! %s\n", strerror(errno));
      continue;
    }

    int i;
    // Cancel some threads
    // printf("curr_queue_len: %d --- num_worker: %d\n", curr_queue_len, num_worker);
    if (curr_queue_len < num_worker) {
      if (num_worker > 1) {
        printf("Deleted %d worker threads because server load decreased\n", num_worker - curr_queue_len - 1);
      }
      for (i = 0; (i < MAX_THREADS) && (curr_queue_len < num_worker) && (num_worker > 1); i++) {
        if (alive_pool[i] == true) {
          alive_pool[i] = false;
          num_worker--;
        }
      }
      pthread_cond_broadcast(&req_queue_notempty);
    }

    // Create new threads
    else if (curr_queue_len > num_worker) {
      pthread_attr_t attr;
      int s;
      s = pthread_attr_init(&attr);
      if (s != 0) {
        fprintf(stderr, "error - attribute init failed.\n");
        exit(EXIT_FAILURE);
      }
      s = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      if (s != 0) {
        fprintf(stderr, "error - setting detached state failed.\n");
        exit(EXIT_FAILURE);
      }
      printf("Created %d worker threads because server load increased\n", curr_queue_len - num_worker);
      for (i = 0; i < MAX_THREADS && curr_queue_len > num_worker; i++) {
        if (alive_pool[i] == false) {
          alive_pool[i] = true;
          s = pthread_create(worker_threads + i, &attr, worker, (void *) &wid_arr[i]);
          if (s != 0) {
            fprintf(stderr, "error - pthread_create for worker threads dynamically.\n");
          }
          num_worker++;
        }
      }
    }

    if (pthread_mutex_unlock(&req_queue_mutex) < 0) {
      printf("Failed to unlock queue! %s\n", strerror(errno));
    }
  }
}
/**********************************************************************************/

/* ************************************ Cache Code ********************************/

// Function to check whether the given request is present in cache
int getCacheIndex(char *request) {
  //printf("Checking cache\n");
  int i;
  for (i = 0; i < cache_size; i++) {
    if (cache[i].request != NULL && strcmp(cache[i].request, request) == 0) {
      //printf("Found in cache %d\n",i);
      return i;
    }
  }
  //printf("Not in cache\n");
  return INVALID;
}

// Function to add the request and its file content into the cache
// It should add the request at an index according to the cache replacement policy you are using
void addIntoCache(char *mybuf, char *memory, int memory_size) {
  //printf("Adding to cache\n");
  if (cache[cache_evict_index].request != NULL) {
    free(cache[cache_evict_index].request);
  }
  if (cache[cache_evict_index].content != NULL) {
    free(cache[cache_evict_index].content);
  }

  //printf("Allocating cache entry\n");
  cache[cache_evict_index].request = (char *) malloc(BUFF_SIZE * sizeof(char));
  cache[cache_evict_index].content = (char *) malloc(memory_size);

  //printf("Filling cache entry\n");
  strcpy(cache[cache_evict_index].request, mybuf);
  memcpy(cache[cache_evict_index].content, memory, memory_size);
  cache[cache_evict_index].len = memory_size;

  cache_evict_index = (cache_evict_index + 1) % cache_size;
  //printf("Cache add done\n");
}

// clear the memory allocated to the cache
void deleteCache() {
  int i;
  for (i = 0; i < cache_size; i++) {
    free(cache[i].request);
    free(cache[i].content);
  }
  free(cache);
}

// Function to initialize the cache
void initCache() {
  cache = malloc(sizeof(cache_entry_t) * cache_size);
  int i;
  for (i = 0; i < cache_size; i++) {
    cache[i].len = 0;
    cache[i].request = NULL;
    cache[i].content = NULL;
  }
}


/**********************************************************************************/

// Function to get the content type from the request
char *getContentType(char *mybuf) {
  int requeue_len = strlen(mybuf);
  char *contenttype;
  if (strcmp(&mybuf[requeue_len - 5], ".html") == 0) {
    contenttype = "text/html";
  } else if (strcmp(&mybuf[requeue_len - 4], ".jpg") == 0) {
    contenttype = "image/jpeg";
  } else if (strcmp(&mybuf[requeue_len - 4], ".gif") == 0) {
    contenttype = "image/gif";
  } else {
    contenttype = "text/plain";
  }
  return contenttype;
}


// Function to open and read the file from the disk into the memory
// You can return the file size if it is useful
int readFromDisk(int fd, char *mybuf, void **memory, int id, int myreqnum, bool connection_persistent) { // *ADDED*
  int filefd;
  if ((filefd = open(mybuf + 1, O_RDONLY)) == -1) {
    // it is mybuf + 1 because get request will return the path with a '/' in the front
    return_error(fd, "Requested file not found.", connection_persistent); // *ADDED*
    // Stop recording the time
    pthread_mutex_lock(&log_mutex);
    printf("[%d][%d][%d][%s][%s][%s]\n", id, myreqnum, fd, mybuf, "Requested file not found.", "MISS");
    fprintf(logfile, "[%d][%d][%d][%s][%s][%s]\n", id, myreqnum, fd, mybuf, "Requested file not found.", "MISS");
    fflush(logfile);
    pthread_mutex_unlock(&log_mutex);
    return -1;
  }
  struct stat st;
  fstat(filefd, &st);
  int filesize = st.st_size;
  *memory = malloc(filesize);
  read(filefd, *memory, filesize);
  close(filefd);
  return filesize;
}


/**********************************************************************************/
// Function to receive the request from the client and put into the queue
void *dispatch(void *arg) {
  int fd;
  char mybuf[BUFF_SIZE];
  char *reqptr;

  char connection_buf[CONNECTION_BUFF_SIZE]; // *ADDED*
  bool connection_open = false; // *ADDED*

  int timer = -1; // *ADDED*

  while (1) {
    if(!connection_open){ // *ADDED*
      fd = accept_connection();
      connection_open = true; // *ADDED*
    }
    else{ // *ADDED* - timer to check how long queue is empty
      /* Need to connect? Incorrect implementation
      if(connect(fd, (struct sockaddr*) &client_addr, addr_size) == -1){
        perror("Could not connect");
      }
      */
      timer++;
      if(timer > 0){
        sleep(1);
      }
      // Close connection if timer is up
      if(timer > 5){ 
        printf("Closing persistent connection...\n");
        close(fd);
        connection_open = false;
        timer = -1;
        continue; // Back to top of loop
      }
    }
    if (fd < 0) { return NULL; }
    if (get_request(fd, mybuf, connection_buf) != 0) { continue; /*back to top of loop!*/ } // *ADDED*

    // *ADDED* - Reset timer if message found
    timer = -1;

    reqptr = (char *) malloc(strlen(mybuf) + 1);
    if (reqptr == NULL) {
      printf("Failed to allocate memory: %s", strerror(errno));
    }
    strcpy(reqptr, mybuf);

    // put request in queue
    if (pthread_mutex_lock(&req_queue_mutex) < 0) {
      printf("Failed to lock queue! %s\n", strerror(errno));
    }

    while (curr_queue_len == queue_len) { // the queue is full!
      pthread_cond_wait(&req_queue_notfull, &req_queue_mutex);
    }

    requests[req_insert_index].fd = fd;
    requests[req_insert_index].request = reqptr;

    // *ADDED* - check connection type
    if(strcmp(connection_buf, "Close") == 0){
      connection_open = false;
      requests[req_insert_index].connection_persistent = false;
    }
    else if(strcmp(connection_buf, "Keep-Alive") == 0){
      requests[req_insert_index].connection_persistent = true;
    }

    curr_queue_len++;
    req_insert_index = (req_insert_index + 1) % queue_len; // ring buffer

    pthread_cond_signal(&req_queue_notempty);
    if (pthread_mutex_unlock(&req_queue_mutex) < 0) {
      printf("Failed to unlock queue! %s\n", strerror(errno));
    }
  }
  return NULL;
}

/**********************************************************************************/

// Function to take the request from the queue, process it and then return a result to the client
void *worker(void *arg) {
  int id = *(int *) arg;
  int fd, filesize, myreqnum;
  void *memory;
  char mybuf[BUFF_SIZE];
  myreqnum = 0;

  bool connection_persistent; // *ADDED*

  while (1) {
    // get request from queue
    if (pthread_mutex_lock(&req_queue_mutex) < 0) {
      printf("Failed to lock! %s\n", strerror(errno));
      continue;
    }

    while (curr_queue_len == 0) { // the queue is empty!
      if (pthread_cond_wait(&req_queue_notempty, &req_queue_mutex) < 0) {
        printf("Failed to wait! %s\n", strerror(errno));
      }
      if (alive_pool[id] == false) {
        if (pthread_mutex_unlock(&req_queue_mutex) < 0) {
          printf("Failed to unlock queue! %s\n", strerror(errno));
        }
        return NULL;
      }
    }

    if (alive_pool[id] == false) {
      if (pthread_mutex_unlock(&req_queue_mutex) < 0) {
        printf("Failed to unlock queue! %s\n", strerror(errno));
      }
      return NULL;
    }

    // Code to get the request from the queue
    fd = requests[req_remove_index].fd;
    connection_persistent = requests[req_remove_index].connection_persistent; // *ADDED*
    strcpy(mybuf, requests[req_remove_index].request);
    free(requests[req_remove_index].request);

    curr_queue_len--;
    req_remove_index = (req_remove_index + 1) % queue_len; // ring buffer

    if (pthread_cond_signal(&req_queue_notfull) < 0) {
      printf("Failed to signal! %s\n", strerror(errno));
    }

    if (pthread_mutex_unlock(&req_queue_mutex) < 0) {
      printf("Failed to unlock! %s\n", strerror(errno));
    }

    myreqnum++;

    if (strcmp(mybuf, "/") == 0) {
      strcpy(mybuf, "/index.html");
    }

    // Code to get the data from cache or from the disk
    bool hit = false;
    if (pthread_mutex_lock(&cache_mutex) < 0) {
      printf("Failed to lock cache mutex! %s\n", strerror(errno));
    }

    int index = getCacheIndex(mybuf);
    if (index != INVALID) {
      filesize = cache[index].len;
      memory = malloc(filesize);
      memcpy(memory, cache[index].content, filesize);
      hit = true;
    } else {
      filesize = readFromDisk(fd, mybuf, &memory, id, myreqnum, connection_persistent); // *ADDED*
      if (filesize == -1) {
        if (pthread_mutex_unlock(&cache_mutex) < 0) {
          printf("Failed to unlock cache mutex! %s\n", strerror(errno));
        }
        continue;
      }
      addIntoCache(mybuf, memory, filesize);
    }

    if (pthread_mutex_unlock(&cache_mutex) < 0) {
      printf("Failed to unlock cache mutex! %s\n", strerror(errno));
    }

    // Code to log the request into the file and terminal
    if (pthread_mutex_lock(&log_mutex) < 0) {
      printf("Failed to lock log mutex! %s\n", strerror(errno));
    }

    printf("[%d][%d][%d][%s][%d][%s]\n", id, myreqnum, fd, mybuf, filesize, (hit == true) ? "HIT" : "MISS");
    if (fprintf(logfile, "[%d][%d][%d][%s][%d][%s]\n", id, myreqnum, fd, mybuf, filesize,
          (hit == true) ? "HIT" : "MISS") < 0) {
      printf("Failed to print to log file! %s\n", strerror(errno));
    }

    fflush(logfile);

    if (pthread_mutex_unlock(&log_mutex) < 0) {
      printf("Failed to unlock log mutex! %s\n", strerror(errno));
    }

    char *contenttype = getContentType(mybuf);
    if (return_result(fd, contenttype, memory, filesize, connection_persistent) != 0) { // *ADDED*
      printf("Couldn't return result, thread id=%d\n", id);
    }

    free(memory);
  }
  return NULL;
}

/**********************************************************************************/

int main(int argc, char **argv) {

  // Error check on number of arguments
  // Decided to check if caching is enabled [argc == 8 -> Caching enabled]
  if (argc != 8) {
    printf("usage: %s port path num_dispatcher num_workers dynamic_flag queue_length cache_size\n", argv[0]);
    return -1;
  }

  // Init variables
  int port = atoi(argv[1]);
  int num_dispatch = atoi(argv[3]);
  num_worker = atoi(argv[4]);
  int dynamic_flag = atoi(argv[5]);
  queue_len = atoi(argv[6]);
  if (atoi(argv[7]) > 0) {
    cache_size = atoi(argv[7]);
  }

  // Sanity error checks
  if (num_dispatch > MAX_THREADS || num_dispatch < 1) {
    fprintf(stderr, "Invalid number of dispatch threads.\n");
    return -2;
  }
  if (num_worker > MAX_THREADS || num_worker < 1) {
    fprintf(stderr, "Invalid number of worker threads.\n");
    return -2;
  }
  if (queue_len > MAX_queue_len || queue_len < 1) {
    fprintf(stderr, "Invalid queue_len size.\n");
    return -2;
  }
  if (cache_size > MAX_CE || cache_size < 1) {
    fprintf(stderr, "Invalid cache_size.\n");
    return -2;
  }

  // Change SIGINT action for grace termination
  struct sigaction act;
  act.sa_handler = setdoneflag;
  act.sa_flags = 0;
  if ((sigemptyset(&act.sa_mask) == -1) || (sigaction(SIGINT, &act, NULL) == -1)) {
    perror("Failed to set SIGINT handler");
    return 1;
  }

  // Open log file
  logfile = fopen("webserver_log", "w");

  // Change the current working directory to server root directory
  if (chdir(argv[2]) != 0) {
    perror("Couldn't change directory to server root.");
    return -1;
  }

  // Init cache if required
  initCache();

  // Start the server
  init(port);
  printf("Starting server on port %d: %d disp, %d work\n", port, num_dispatch, num_worker);

  // Create dispatcher and worker threads
  int i;
  for (i = 0; i < MAX_THREADS; i++) {
    alive_pool[i] = false;
  }
  for (i = 0; i < num_worker; i++) {
    wid_arr[i] = i;
  }
  for (i = 0; i < num_dispatch; i++) {
    did_arr[i] = i;
  }

  pthread_attr_t attr;
  int s;

  s = pthread_attr_init(&attr);
  if (s != 0) {
    fprintf(stderr, "error - attribute init failed.\n");
    exit(EXIT_FAILURE);
  }
  s = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (s != 0) {
    fprintf(stderr, "error - setting detached state failed.\n");
    exit(EXIT_FAILURE);
  }

  for (i = 0; i < num_worker; i++) {
    alive_pool[i] = true;
    s = pthread_create(worker_threads + i, &attr, worker, (void *) &wid_arr[i]);
    /* Alternative */
    //        s = pthread_create(worker_threads + i, NULL, worker, (void *) &wid_arr[i]);
    //        s = pthread_detach(*(worker_threads + i));
    if (s != 0) {
      fprintf(stderr, "error - pthread_create for worker threads.\n");
      exit(EXIT_FAILURE);
    }
  }
  for (i = 0; i < num_dispatch; i++) {
    s = pthread_create(dispatch_threads + i, &attr, dispatch, (void *) &did_arr[i]);
    if (s != 0) {
      fprintf(stderr, "error - pthread_create for dispatcher threads.\n");
      exit(EXIT_FAILURE);
    }
  }

  pthread_t dynamic_pool_deamon;
  if (dynamic_flag == 1) {
    int dynamic_pool_deamon_id = MAX_THREADS + 1;
    s = pthread_create(&dynamic_pool_deamon, &attr, dynamic_pool_size_update, (void *) &dynamic_pool_deamon_id);
    if (s != 0) {
      fprintf(stderr, "error - pthread_create for dynamic pool management.\n");
      exit(EXIT_FAILURE);
    }
  }

  // Terminate server gracefully
  while (!doneflag) {
    sleep(1);
  }

  printf("Server terminating ...\n");

  // Print the number of pending requests in the request queue
  printf("The number of pending requests in the request queue: %d\n", curr_queue_len);


  if (pthread_mutex_lock(&log_mutex) < 0) {
    printf("Failed to lock log mutex! %s\n", strerror(errno));
  }

  // close log file
  fclose(logfile);

  if (pthread_mutex_unlock(&log_mutex) < 0) {
    printf("Failed to unlock log mutex! %s\n", strerror(errno));
  }



  if (pthread_mutex_lock(&cache_mutex) < 0) {
    printf("Failed to lock cache mutex! %s\n", strerror(errno));
  }

  // Remove cache
  deleteCache();

  if (pthread_mutex_unlock(&cache_mutex) < 0) {
    printf("Failed to unlock cache mutex! %s\n", strerror(errno));
  }

  printf("Main thread exiting.\n");
  return 0;
}
