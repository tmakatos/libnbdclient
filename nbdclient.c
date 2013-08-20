/*
 * Wrapper for open, read, and close libc functions.
 */
#define _GNU_SOURCE
#include <dlfcn.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <assert.h>
#include <strings.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <search.h>
#include <inttypes.h>
#include <sys/un.h>
#include <stdlib.h>
#include <sys/param.h>

#ifndef _BSD_SOURCE
#define _BSD_SOURCE
#endif
#include <endian.h>

//#define DEBUG 1

#if DEBUG == 1
#define INFO(_f, _a...) printf("libnbdclient: %s:%d: " _f, \
		__FILE__, __LINE__, ##_a)

#define ERROR(_f, _a...) fprintf(stderr, "libnbdclient: %s:%d: " _f, \
		__FILE__, __LINE__, ##_a)
#else
#define INFO(_f, _a...) do {} while(0)
#define ERROR(_f, _a...) do {} while(0)
#endif

#define NBD_MAGIC 0x00420281861253
#define NBD_REQUEST_MAGIC 0x25609513
#define NBD_REPLY_MAGIC 0x67446698
#define NBD_CMD_READ 0

#define SECTOR_SHIFT 9
#define SECTOR_SIZE (1 << SECTOR_SHIFT)
#define SECTOR_MASK (SECTOR_SIZE - 1)

void *open_fds = NULL;

struct nbdfd {
	/**
	 * The socket file descriptor.
	 */
	int sock;

	uint64_t offset;
	uint64_t size;
	struct sockaddr_un remote;
};

static int compar(const void *a, const void *b) {

	const struct nbdfd *_a = a;
	const struct nbdfd *_b = b;

	assert(_a);
	assert(_b);

	if (_a->sock < _b->sock)
		return -1;
	else if (_a->sock > _b->sock)
		return 1;
	else
		return 0;
}

static int myrecv(int fd, void *buf, size_t size,
		const char *s __attribute__((unused))) {

	size_t err = 0;

	assert(fd != -1);
	assert(buf);

	err = recv(fd, buf, size, 0);
	if (err != size) {
		if (err == (size_t)-1) {
			ERROR("failed to receive in %s: %s\n", s, strerror(errno));
			return -1;
		} else {
			ERROR("received only %u of %u in %s\n", err, size, s);
			return -1;
		}
	}
	return 0;
}

int nbd_open(const char *sockpath, int _flags __attribute__((unused))) {

	int err = 0;
	uint64_t magic = 0;
	uint32_t flags = 0;
	char buf[BUFSIZ];
	int i = 0;
	int len = 0;
	struct nbdfd *nbdfd = NULL;

	if (!(nbdfd = malloc(sizeof(struct nbdfd)))) {
		err = ENOMEM;
		goto out;
	}

	bzero(nbdfd, sizeof(*nbdfd));

	if ((nbdfd->sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		err = errno;
		ERROR("failed to create socket: %s\n", strerror(err));
		goto out;
	}

	nbdfd->remote.sun_family = AF_UNIX;
    strcpy(nbdfd->remote.sun_path, sockpath);
    len = strlen(nbdfd->remote.sun_path) + sizeof(nbdfd->remote.sun_family);
    if ((err = connect(nbdfd->sock, (struct sockaddr *)&nbdfd->remote, len))
			== -1) {
		err = errno;
		ERROR("failed to connect to %s: %s\n", sockpath, strerror(err));
		goto out;
	}

	if (myrecv(nbdfd->sock, buf, 8, "\"NBDMAGIC\"")) {
		err = EINVAL;
		goto out;
	}

	if (strncmp(buf, "NBDMAGIC", 8)) {
		ERROR("got %.8s instead of NBDMAGIC\n", buf);
		err = EINVAL;
		goto out;
	}

	if (myrecv(nbdfd->sock, &magic, sizeof(magic), "NBDMAGIC")) {
		err = EINVAL;
		goto out;
	}
	magic = be64toh(magic);
	if (magic != NBD_MAGIC) {
		ERROR("got %llu instead of %llu\n", magic, NBD_MAGIC);
		err = EINVAL;
		goto out;
	}

	if (myrecv(nbdfd->sock, &nbdfd->size, sizeof(nbdfd->size), "size")) {
		err = EINVAL;
		goto out;
	}
	nbdfd->size = be64toh(nbdfd->size);
	if (myrecv(nbdfd->sock, &flags, sizeof(flags), "flags")) {
		err = EINVAL;
		goto out;
	}
	flags = be32toh(flags);
	if (myrecv(nbdfd->sock, buf, 124, "124 zeros")) {
		err = EINVAL;
		goto out;
	}

	for (i = 0; i < 124; i++) {
		if (buf[i] != 0) {
			ERROR("byte 0x%x is 0x%x instead of 0x0\n", i, buf[i]);
			err = EINVAL;
			goto out;
		}
	}

	tsearch(nbdfd, &open_fds, &compar);

out:
	if (err) {
		free(nbdfd);
		return -1;
	}

	INFO("opened %s as %d\n", sockpath, nbdfd->sock);

	return nbdfd->sock;
}

struct read_request {
	uint32_t magic;
	uint32_t command;
	uint64_t handle;
	uint64_t offset;
	uint32_t length;
} __attribute__((packed));

struct reply {
	uint32_t magic;
	uint32_t ec;
	uint64_t handle;
} __attribute__((packed));

int nbd_read_aligned(int fd, void *buf, size_t count, off64_t offset) {
	ssize_t err = 0;
	size_t pending = count;
	struct read_request read_request;
	struct reply reply;

	read_request.magic = htobe32(NBD_REQUEST_MAGIC);
	read_request.command = htobe32(NBD_CMD_READ);
	read_request.handle = htobe64(0);

	if ((count & SECTOR_MASK) != 0 || (offset & SECTOR_MASK) != 0) {
		ERROR("misaligned read 0x%x@0x%016llX\n", count, (uint64_t)offset);
		errno = EINVAL;
		return -1;
	}

	while (pending) {
		/*
		 * FIXME Where's this defined in tapdisk?
		 */
		size_t _count = MIN(pending, 1 << 12);
		read_request.offset = htobe64(offset + count - pending);
		read_request.length = htobe32(_count);
		if ((err = send(fd, &read_request, sizeof(read_request), 0))
				!= sizeof(read_request)) {
			if (err == -1)
				ERROR("failed to send: %s\n", strerror(errno));
			else
				ERROR("sent %u instead of %u\n", err, sizeof(read_request));
			return -1;
		}
		if (myrecv(fd, &reply, sizeof(reply),
					"receive read reply header") != 0)
			return -1;
		reply.magic = be32toh(reply.magic);
		if (reply.magic != NBD_REPLY_MAGIC || reply.ec != 0
				|| reply.handle != 0) {
			ERROR("invalid reply header\n"
					"\t\texpected\tactual\n"
					"magic\t\t0x%x\t0x%x\n"
					"err code\t0\t\t%s\n"
					"handle\t\t0x0\t\t0x%016llX\n",
					NBD_REPLY_MAGIC, reply.magic,
					reply.ec ? "0" : strerror(reply.ec), reply.handle);
			return -1;
		}
		if (myrecv(fd, buf + count - pending, _count,
					"receiving read data") != 0)
			return -1;
		pending -= _count;
	}
	return 0;
}

int nbd_read(struct nbdfd *nbdfd, void *buf, size_t count, uint64_t *poffset) {

	uint64_t _offset;
	uint32_t _length;
	void *_buf = buf;
	ssize_t err = 0;

	assert(nbdfd);

	if (count == 0)
		goto out;

	if (!poffset)
		poffset = &nbdfd->offset;

	_offset = *poffset;
	_length = count;

	if ((_offset & SECTOR_MASK) != 0) {
		_offset -= *poffset & SECTOR_MASK;
		_length += *poffset & SECTOR_MASK;
	}

	if ((_length & SECTOR_MASK) != 0)
		_length = (_length & ~SECTOR_MASK) + SECTOR_SIZE;

	if (_offset + _length > nbdfd->size)
		_length = nbdfd->size - _offset;

	INFO("%d: NBD read %u@%llu\n", nbdfd->sock, _length, _offset);

	if (_length != count)
		if (!(_buf = malloc(_length))) {
			ERROR("failed to allocate %u bytes for the reception buffer\n",
					_length);
			err = ENOMEM;
			goto out;
		}

	if((err = nbd_read_aligned(nbdfd->sock, _buf, _length, _offset)) != 0) {
		err = errno;
		ERROR("failed to read from server: %s\n", strerror(err));
		goto out;
	}

	/* relative buffer offset */
	_offset = *poffset - _offset;
	if (buf != _buf)
		memcpy(buf, _buf + _offset, _offset + count);
	*poffset += count;

out:
	if (_buf != buf)
		free(_buf);
	if (!err)
		return count;
	else
		return -1;
}

int open(const char *pathname, int flags) {
	int (*original_open)(const char*, int) = dlsym(RTLD_NEXT, "open");
	struct stat buf;
	int err = 0;

	assert(original_open);

	INFO("opening %s\n", pathname);

	bzero(&buf, sizeof(buf));
	err = stat(pathname, &buf);

	if (err != -1 && S_ISSOCK(buf.st_mode))
		return nbd_open(pathname, flags);
	else
		return original_open(pathname, flags);
}

/*
 * FIXME open64 is supposed to pass O_LARGEFILE but it seems it gets translated
 * to a directory flag.
 */
int open64(const char *pathname, int flags) {
	/* FIXME O_LARGEFILE == 0200000? */
	return open(pathname, flags);
}

ssize_t do_read(int fd, void *buf, size_t count, off_t *offset) {
	int (*original_read)(int, void*, size_t) = dlsym(RTLD_NEXT, "read");
	int (*original_pread)(int, void*, size_t, off_t)
		= dlsym(RTLD_NEXT, "pread");
	struct nbdfd **nbdfd, _nbdfd = {.sock = fd};

	nbdfd = tfind(&_nbdfd, &open_fds, compar);
	if (nbdfd) {
		uint64_t _offset;
		uint64_t *_poffset;

		if (offset) {
			_offset = (unsigned)*offset;
			_poffset = &_offset;
		} else
			_poffset = NULL;

		return nbd_read(*nbdfd, buf, count, _poffset);
	} else
		if (offset)
			return original_pread(fd, buf, count, *offset);
		else
			return original_read(fd, buf, count);
}

ssize_t read(int fd, void *buf, size_t count) {

	INFO("%d: libc::read %d\n", fd, count);

	return do_read(fd, buf, count, NULL);
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {

	INFO("%d: libc::pread %d@%lu\n", fd, count, offset);

	return do_read(fd, buf, count, &offset);
}

ssize_t pread64(int fd, void *buf, size_t count, off_t offset) {

	INFO("%d: libc::pread64 %d@%lu\n", fd, count, offset);

	return pread(fd, buf, count, offset);
}


static int nbd_close(struct nbdfd *nbdfd) {
	int (*original_close)(int) = dlsym(RTLD_NEXT, "close");
	int err = 0;

	assert(nbdfd);

	if (original_close(nbdfd->sock) != 0) {
		err = errno;
		ERROR("failed to close fd %d: %s\n", nbdfd->sock, strerror(err));
		return err;
	}

	tdelete(nbdfd, &open_fds, compar);

	return 0;
}

int close(int fd) {
	int (*original_close)(int) = dlsym(RTLD_NEXT, "close");
	struct nbdfd **nbdfd, _nbdfd = {.sock = fd};

	nbdfd = tfind(&_nbdfd, &open_fds, compar);
	if (nbdfd)
		return nbd_close(*nbdfd);
	else
		return original_close(fd);

}

off64_t nbd_seek(struct nbdfd *nbdfd, off64_t offset, int whence) {
	assert(nbdfd);

	INFO("%d: lseek64 to %llu from %d\n", nbdfd->sock, offset, whence);

	switch (whence) {
		case SEEK_SET:
			/* FIXME Is it legal to set it to EOF? */
			if (offset >= (off64_t)nbdfd->size)
				return EINVAL;
			nbdfd->offset = offset;
			break;
		case SEEK_CUR:
			if (nbdfd->offset + offset >= nbdfd->size)
				return EINVAL;
			nbdfd->offset += offset;
			break;
		case SEEK_END:
			return ESPIPE;
		default:
			return EINVAL;
	}
	return nbdfd->offset;
}

off64_t lseek64(int fd, off64_t offset, int whence) {
	off64_t (*original_seek)(int, off64_t, int) = dlsym(RTLD_NEXT, "lseek64");
	struct nbdfd _nbdfd = {.sock = fd},
				 **nbdfd = tfind(&_nbdfd, &open_fds, compar);

	if (nbdfd)
		return nbd_seek(*nbdfd, offset, whence);
	else
		return original_seek(fd, offset, whence);
}

off_t lseek(int fd, off_t offset, int whence) {

	INFO("%d: lseek at %ld from %d\n", fd, offset, whence);

	return lseek64(fd, offset, whence);
}
