CC=gcc
CFLAGS=-Wall -O3

all: nio
nio: nio.o
clean:
	rm nio.o nio

.PHONY: clean
