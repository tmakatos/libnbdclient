nbdclient
=========

The nbdclient library implement the client side of the NBD protocol (http://nbd.sourceforge.net/). It allows applications to read to and write from NBD servers listening to a UNIX domain socket. CUrrently only read funcionality is supported, and for specific NBD protocol versions. Along with the NBD funcionality, there's funcionality that intercepts common file operation calls (e.g. open, read, close) and if they're directed to a UNIX domain socket, it replaces them with appropriate NBD calls such that the application doesn't need modifications.

This library is in it's early developemnt phase.

The functionality of intercepting the libc calls should be moved into another repository.
