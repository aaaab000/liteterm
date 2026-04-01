CC = gcc
CFLAGS = -O2 -Wall -Wextra -pedantic \
         $(shell pkg-config --cflags fontconfig xft)
LDFLAGS = -lX11 -lutil \
          $(shell pkg-config --libs fontconfig xft)
PREFIX = /usr/local

SRC = liteterm.c
BIN = liteterm

all: $(BIN)

$(BIN): $(SRC) config.h
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)

clean:
	rm -f $(BIN)

install: $(BIN)
	install -Dm755 $(BIN) $(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(PREFIX)/bin/$(BIN)

.PHONY: all clean install uninstall