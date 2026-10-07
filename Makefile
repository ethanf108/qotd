CC=gcc
BIN=qotd
SOURCE_FILES=*.c
INSTALL_PATH=/usr/local/bin/
CFLAGS=-O2

build:
	@$(CC) $(CFLAGS) -o $(BIN) $(SOURCE_FILES)

clean:
	@rm -v $(BIN)

install:
	@install -t $(INSTALL_PATH) $(BIN)
