CC = gcc
CFLAGS = -D_REENTRANT
LDFLAGS = -lpthread -pthread

web_server: server.c util.c
	${CC} -Wall -o web_server server.c util.c ${LDFLAGS}

clean:
	rm web_server
