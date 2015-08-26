CC=gcc
CFLAGS=-g -Wall -O3
LDFLAGS=-lpthread

all: nio
nio: nio.o
clean:
	rm nio.o nio

.PHONY: clean
