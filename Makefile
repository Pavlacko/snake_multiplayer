CC=gcc
CFLAGS=-Wall -Wextra -Werror -O2 -std=c11
NCURSES=-lncurses
PTHREAD=-lpthread

SERVER_BIN=server/server
CLIENT_BIN=client/client

COMMON_SRC=common/net.c
SERVER_SRC=server/server.c
CLIENT_SRC=client/client.c

.PHONY: all server client clean

all: server client

server: $(SERVER_SRC) $(COMMON_SRC)
	$(CC) $(CFLAGS) -o $(SERVER_BIN) $(SERVER_SRC) $(COMMON_SRC) $(PTHREAD)

client: $(CLIENT_SRC) $(COMMON_SRC)
	$(CC) $(CFLAGS) -o $(CLIENT_BIN) $(CLIENT_SRC) $(COMMON_SRC) $(NCURSES)

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN) common/*.o server/*.o client/*.o *.o
