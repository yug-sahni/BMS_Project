CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-result
CFLAGS  += -Iinclude
LDLIBS  = -lcrypt

BIN     = bin
COMMON  = src/store.c src/net.c src/auth.c src/common_ops.c
SERVER  = src/server.c src/session.c src/journal.c src/customer.c src/employee.c src/manager.c src/admin.c
HEADERS = $(wildcard include/*.h)

all: $(BIN)/bms_server $(BIN)/bms_client $(BIN)/bms_init $(BIN)/bms_migrate

$(BIN):
	mkdir -p $(BIN)

$(BIN)/bms_server: $(SERVER) $(COMMON) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(SERVER) $(COMMON) $(LDLIBS)

$(BIN)/bms_init: src/bms_init.c $(COMMON) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/bms_init.c $(COMMON) $(LDLIBS)

$(BIN)/bms_migrate: src/bms_migrate.c $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/bms_migrate.c

$(BIN)/bms_client: src/client.c include/bms.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/client.c

test: all
	python3 tests/test_bms.py

clean:
	rm -rf $(BIN)

.PHONY: all test clean
