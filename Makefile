XEN_ROOT = $(CURDIR)/../..
include $(XEN_ROOT)/tools/Rules.mk

MAJOR = 0
MINOR = 1
NAME = libnbdclient
VERSION = $(MAJOR).$(MINOR)
LIB = $(NAME).so.$(VERSION)

$(LIB): nbdclient.c
	$(CC) -fPIC -shared -Wl,-soname,$(NAME).so.$(MAJOR) -ldl $^ -o $@

.PHONY: all
all: build

build: $(LIB)

.PHONY: install
intall:
	$(INSTALL_PROG) $(LIB) $(DESTDIR)$(LIBDIR)

.PHONY: clean
clean:
	rm -f $(LIB)
