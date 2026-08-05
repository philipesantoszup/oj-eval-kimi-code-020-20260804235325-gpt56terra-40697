CC := gcc

.PHONY: all clean

all: code

code: main.c buddy.c buddy.h utils.h
	$(CC) -w -std=c11 -O2 -o $@ main.c buddy.c

clean:
	rm -f code test
