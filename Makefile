all: libnbdclient.so test

CC=gcc
CFLAGS=-Wall -Wextra -Werror -O0 -g -m32

libnbdclient.so: nbdclient.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ nbdclient.c -ldl

test: test.c
	$(CC) $(CFLAGS) test.c -o test /var/tmp/libfsimage.so.1.0.0 -ldl

.PHONY: clean

clean:
	rm -f libnbdclient.so test
