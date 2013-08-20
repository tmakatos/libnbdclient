#!/usr/bin/python

import socket
import sys
import struct
import os
import stat
import unittest
import tempfile
import subprocess

class NBDClient(object):

    OLD_NEGOTIATION_MAGIC = 0x00420281861253
    NEW_NEGOTIATION_MAGIC = 0x49484156454F5054
    NBD_REQUEST_MAGIC = 0x25609513
    NBD_REPLY_MAGIC = 0x67446698

    class NBD_OPT:
        NBD_OPT_EXPORT_NAME = 1


    class NBD_CMD:
        NBD_CMD_READ = 0
        NBD_CMD_WRITE = 1
        NBD_CMD_DISC = 2
        NBD_CMD_FLUSH = 3
        NBD_CMD_TRIM = 4


    class NBDException(Exception):
        def __init__(self, msg):
            Exception.__init__(self, msg)


    class NBDNegotiationException(NBDException):
        pass


    class NBDDataException(NBDException):
        pass


    def __init__(self, host, sock_type, port, export):
        assert host
        if sock_type == socket.AF_INET:
            self.__conn = host
            if not port:
                port = 10809
                self.__conn = (host, port)
        elif sock_type == socket.AF_UNIX:
            assert not port
            self.__conn = host
        else:
            raise Exception('invalid socket type %s' % sock_type)

        self.__sock_type = sock_type
        self.__export = export
        self.__sock = None
        self.__connected = False
        self.__size = None
        self.__flags = None

    def _connect_old(self):
        reply = self.__sock.recv(8)
        self.__size = struct.unpack('>Q', reply)[0]
        reply = self.__sock.recv(4)
        self.__flags = struct.unpack('>I', reply)[0]
        reply = self.__sock.recv(124)
        # FIXME check that reply is 124 bytes of zeros

    def _connect_new(self):
        reply = self.__sock.recv(2)
        reply = struct.unpack('>H', reply)[0]
        assert reply == 0

        self.__sock.sendall(struct.pack('>I', 0))
        self.__sock.sendall(struct.pack('>Q', NBDMAGIC))
        self.__sock.sendall(struct.pack('>I', NBD_OPT.NBD_OPT_EXPORT_NAME))
        self.__sock.sendall(struct.pack('>I', len(self.__export)))
        self.__sock.sendall(self.__export)

        reply = self.__sock.recv(8)
        self.__size = struct.unpack('>Q', reply)[0]

        reply = self.__sock.recv(2)
        self.__flags = struct.unpack('>H', reply)[0]

        reply = self.__sock.recv(124)
        # FIXME check that reply is 124 bytes of zeros

    def connect(self):
        assert not self.__connected
        self.__sock = socket.socket(self.__sock_type, socket.SOCK_STREAM)
        self.__sock.connect(self.__conn)
        reply = self.__sock.recv(8)
        if reply != 'NBDMAGIC':
            raise NBDNegotiationException('got %s instead of NBDMAGIC' % reply)
        reply = self.__sock.recv(8)
        reply = struct.unpack('>Q', reply)[0]
        if reply == self.OLD_NEGOTIATION_MAGIC:
            self._connect_old()
        elif reply == self.NEW_NEGOTIATION_MAGIC:
            self._connect_new()
        else:
            raise self.NBDNegotiationException('invalid NBDMAGIC %s' \
                    % hex(reply))

        self.__connected = True

    def disconnect(self):
        self.__connected = False
        req = struct.pack('>I', self.NBD_REQUEST_MAGIC) \
                + struct.pack('>I', self.NBD_CMD.NBD_CMD_DISC) \
                + struct.pack('>Q', 0) \
                + struct.pack('>Q', 0) \
                + struct.pack('>I', 0)
        self.__sock.sendall(req)
        self.__sock.close()

    def read(self, offset, length):
        assert self.__connected
        req = struct.pack('>I', self.NBD_REQUEST_MAGIC) \
                + struct.pack('>I', self.NBD_CMD.NBD_CMD_READ) \
                + struct.pack('>Q', 0) \
                + struct.pack('>Q', offset) \
                + struct.pack('>I', length)
        self.__sock.sendall(req)
        # magic (32 bits) + error code (32 bits) + handle (64 bits)
        rsp = self.__sock.recv(16)
        if len(rsp) != 16:
            raise NBDDataException('error receiving read reply: got %s bytes '
                    'instead of 16' % len(rsp))
        magic = struct.unpack('>I', rsp[0:4])[0]
        ec = struct.unpack('>I', rsp[4:8])[0]
        handle = struct.unpack('>Q', rsp[8:16])[0]
        if magic != self.NBD_REPLY_MAGIC:
            raise self.NBDDataException('got %s instead of %s' % (hex(magic), \
                    hex(self.NBD_REPLY_MAGIC)))
        assert ec == 0
        assert handle == 0
        rsp = self.__sock.recv(length)
        return rsp

    def size(self):
        return self.__size


class NBDFileWrapper(file):

    SECTOR_SIZE = 512

    def __init__(self, sockpath):
        self.__nbdc = NBDClient(sockpath, socket.AF_UNIX, None, None)
        self.__nbdc.connect()
        self.__closed = False
        self.__offset = 0

    def seek(self, offset, whence=0):
        if whence and whence != 0:
            raise NotImplemented('whence can only be 0')
        assert offset >= 0 # FIXME raise IOError: [Errno 22] Invalid argument
        self.__offset = offset

    def read(self, length):
        if length == 0:
            return ''
        l = length
        o = self.__offset
        if self.__offset % self.SECTOR_SIZE != 0:
            o -= self.__offset % self.SECTOR_SIZE
            l += self.__offset % self.SECTOR_SIZE
        if l < self.SECTOR_SIZE:
            l = self.SECTOR_SIZE
        elif l % self.SECTOR_SIZE != 0:
            l = (l / self.SECTOR_SIZE) * self.SECTOR_SIZE + self.SECTOR_SIZE
            if o + l > self.__nbdc.size():
                l = self.__nbdc.size() - o
        data = self.__nbdc.read(o, l)
        assert len(data) >= 0 and len(data) <= l
        o = self.__offset - o
        if o >= len(data):
            return ''
        if o + length > len(data):
            length = len(data) - o
        self.__offset += length
        return data[o:(o + length)]

    def close(self):
        self.__nbdc.disconnect()
        self.__closed = True


def nbdopen(name, mode='r', buffering=-1):
    if stat.S_ISSOCK(os.stat(name).st_mode):
        if (mode != None and mode != 'r') or buffering != -1:
            raise NotImplemented('mode != r or buffering != None unsupported')
        return NBDFileWrapper(name)
    else:
        return open(name=name, mode=mode, buffering=buffering)

class Test_nbd(unittest.TestCase):

    # FIXME no check for exceptions and clean up

    FILE_SIZE = 1 << 14

    def __init__(self, *args, **kwargs):
        unittest.TestCase.__init__(self, *args, **kwargs)
        [fd, self.__tempfile] = tempfile.mkstemp()
        self.__data = os.urandom(self.FILE_SIZE)
        os.write(fd, self.__data)
        os.close(fd)

        child = subprocess.Popen(['tap-ctl', 'spawn'], stdout=subprocess.PIPE)
        stdout = child.stdout.read().strip()
        rc = child.wait()
        assert rc == 0
        self.__tapdisk_pid = stdout.split()[-1]
        self.__sockpath = '/var/run/blktap-control/nbd%s' % self.__tapdisk_pid

        rc = subprocess.call(['tap-ctl', 'open', '-p', self.__tapdisk_pid,
            '-a','aio:%s' % self.__tempfile])
        assert rc == 0

    def __del__(self):
        os.unlink(self.__tempfile)
        rc = subprocess.call(['tap-ctl', 'destroy', '-p', self.__tapdisk_pid,
            '-a', 'aio:%s' % self.__tempfile])
        assert rc == 0

    def setUp(self):
        pass

    def tearDown(self):
        pass

    def test_basic(self):
        for path in (self.__tempfile, self.__sockpath):
            fp = nbdopen(path)
            d = fp.read(0)
            self.assertEqual(d, '')
            d = fp.read(1)
            self.assertEqual(d, self.__data[0], path)
            d = fp.read(1)
            self.assertEqual(d, self.__data[1], path)
            fp.seek(3)
            d = fp.read(1)
            self.assertEqual(d, self.__data[3], path)
            fp.seek(0)
            d = fp.read(512)
            self.assertEqual(d, self.__data[0:512], path)
            fp.seek(0)
            d = fp.read(1024)
            self.assertEqual(d, self.__data[0:1024], path)
            fp.seek(512)
            d = fp.read(1)
            self.assertEqual(d, self.__data[512], path)
            fp.seek(0)
            d = fp.read(513)
            self.assertEqual(d, self.__data[0:513], path)
            fp.seek(self.FILE_SIZE - 1)
            d = fp.read(2)
            self.assertEqual(d, self.__data[self.FILE_SIZE - 1], path)

if __name__ == '__main__':
    unittest.main()
