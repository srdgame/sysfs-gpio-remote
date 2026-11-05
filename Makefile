CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11 -pthread
CFLAGS  += -MMD -MP -Icommon -Iserver -Iclient

COMMON_SRC := common/proto_io.c common/log.c

SERVER_SRC := $(COMMON_SRC) \
	server/main.c \
	server/connection.c \
	server/gpio_backend.c \
	server/edge_thread.c

CLIENT_SRC := $(COMMON_SRC) \
	client/main.c \
	client/fuse_ops.c \
	client/rpc.c \
	client/state.c

SERVER_OBJ := $(SERVER_SRC:.c=.o)
CLIENT_OBJ := $(CLIENT_SRC:.c=.o)

SERVER_CFLAGS  := $(CFLAGS) $(shell pkg-config --cflags libgpiod)
SERVER_LIBS    := $(shell pkg-config --libs libgpiod)
CLIENT_CFLAGS  := $(CFLAGS) $(shell pkg-config --cflags fuse3)
CLIENT_LIBS    := $(shell pkg-config --libs fuse3)

BIN_SERVER := gpio-remote-server
BIN_CLIENT := gpio-remote-client
BIN_TEST   := test/proto_test test/poll_helper

.PHONY: all server client test clean

all: server client test

server: $(BIN_SERVER)
client: $(BIN_CLIENT)
test: $(BIN_TEST)

test/poll_helper: test/poll_helper.c
	$(CC) $(CFLAGS) -o $@ $<

$(BIN_SERVER): $(SERVER_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(SERVER_LIBS)

$(BIN_CLIENT): $(CLIENT_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(CLIENT_LIBS)

server/%.o: server/%.c
	$(CC) $(SERVER_CFLAGS) -c -o $@ $<

client/%.o: client/%.c
	$(CC) $(CLIENT_CFLAGS) -c -o $@ $<

common/%.o: common/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

test/proto_test: test/proto_test.c common/proto_io.o common/log.o
	$(CC) $(CFLAGS) -Icommon -o $@ $^

clean:
	rm -f $(SERVER_OBJ) $(CLIENT_OBJ) $(BIN_SERVER) $(BIN_CLIENT) $(BIN_TEST)
	rm -f common/*.d server/*.d client/*.d test/*.o test/*.d

-include $(SERVER_OBJ:.o=.d) $(CLIENT_OBJ:.o=.d) test/*.d
