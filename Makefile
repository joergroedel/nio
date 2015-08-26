CC=gcc
CFLAGS=-pthread -g -Wall -O3
LDFLAGS=-pthread

all: nio
nio: nio.o
clean:
	rm nio.o nio

.PHONY: clean
