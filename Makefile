SHELL = /bin/sh

UNAME := $(shell uname)
ifeq ($(UNAME), Linux)
	CFLAGS = -D_DEFAULT_SOURCE -D_BSD_SOURCE
else
	# macOS / *BSD
	CFLAGS = -include bsd.h
endif

all:
	$(CC) $(CFLAGS) -std=c99 -pthread -pedantic -Wall -o HttpStream2Udp main.c
