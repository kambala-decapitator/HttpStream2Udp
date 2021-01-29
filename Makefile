SHELL = /bin/sh

override CFLAGS += -std=c99 -pedantic -D_DEFAULT_SOURCE -D_BSD_SOURCE

all:
	$(CC) $(CFLAGS) -o HttpStream2Udp main.c
