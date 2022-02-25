# multi-threaded-web-server
The program utilizes networks to connect the clients to
the host for the webserver. This is done first by initializing
the servers main socket in the init() function. For each request
made by the client, an additional socket will be created to
establish direct communication between the individual client
and the sever. After the server recieves the message from the
client, get_request() gets the filename to later be read from
the disk or the cache.

### Compile and Run

To compile the program in a linux environment, type

> ```> make``` 

in the terminal. To run the program type

> ```> ./web_server <port> <path_to_testing>/testing <num_dispatch> <num_worker> <dynamic_flag> <queue_len> <cache_entries>```

This solution implements dynamic_flag and a cache, so use 
1 and 100 respectively. 

To test the server, open another terminal and type

> ```> cat <path_to_urls_file> | xargs -n 1 -P 8 wget --no-http-keep-alive```