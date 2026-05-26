CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

.PHONY: all install uninstall clean test

all: jenv

jenv: jenv.c
	$(CC) $(CFLAGS) jenv.c -o jenv

install: jenv
	mkdir -p $(BINDIR)
	cp jenv $(BINDIR)/jenv

uninstall:
	rm -f $(BINDIR)/jenv

clean:
	rm -f jenv

test: jenv
	bash ./test.sh
