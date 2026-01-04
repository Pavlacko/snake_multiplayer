CC=gcc
CFLAGS=-Wall -Wextra -Werror -O2 -std=c11
PTHREAD=-lpthread
NCURSES=-lncurses

COMMON_SRC=common/net.c
SERVER_SRC=server/server.c
CLIENT_SRC=client/client.c

all: server_bin client_bin

server_bin: $(SERVER_SRC) $(COMMON_SRC)
	$(CC) $(CFLAGS) -o server/server $(SERVER_SRC) $(COMMON_SRC) $(PTHREAD)

client_bin: $(CLIENT_SRC) $(COMMON_SRC)
	$(CC) $(CFLAGS) -o client/client $(CLIENT_SRC) $(COMMON_SRC) $(NCURSES)

clean:
	rm -f server/server client/client *.o common/*.o server/*.o client/*.o
