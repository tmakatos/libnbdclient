all: libnbdclient.so

CC=gcc
CFLAGS=-Wall -Wextra -Werror -O0 -g -m32

libnbdclient.so: nbdclient.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ nbdclient.c -ldl

.PHONY: clean

clean:
	rm -f libnbdclient.so test
