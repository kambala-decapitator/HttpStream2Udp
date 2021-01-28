SHELL = /bin/sh

override CFLAGS += -std=c99 -pedantic

all:
	$(CC) $(CFLAGS) -o HttpStream2Udp main.c
