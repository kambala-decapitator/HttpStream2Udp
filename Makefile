SHELL = /bin/sh

CFLAGS = -std=c99 -Wpedantic

all:
	$(CC) $(CFLAGS) -o HttpStream2Udp main.c
