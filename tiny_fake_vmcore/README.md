tiny fake vmcore
================

This is a quick tool to create a very tiny, but valid, vmcore file. It is based
on a tool inside the libkdumpfile test suite by Petr Tesarik, so I can't take
any credit for the implementation.

The important part is that we can control the osrelease & vmcoreinfo, and that
common tools like drgn, kdumpid, strings, etc will be able to see that kernel
version and behave nicely.

This is useful for me because I need to test the automatic debuginfo extraction.
So I need a vmcore that has a kernel version I can control, but I need to copy
it to a restricted environment and I don't want to constantly be crashing VMs
and copying hundreds of MiB around. I don't actually care to *debug* the vmcore,
I just want the tools to use it.

Setup
-----

First, clone libkdumpfile and ensure you have its build dependencies, then build
the "mkdiskdump" test utility.

``` sh
dnf install git gcc make automake autoconf libtool pkgconfig libzstd-devel \
            binutils-devel lzo-devel python3-devel python3-setuptools \
            snappy-devel zlib-devel
git clone https://github.com/ptesarik/libkdumpfile v0.5.5

cd libkdumpfile/
autoreconf -fi
./configure
cd tests
make mkdiskdump
```

The resulting `mkdiskdump` program should be copied into this directory.
You are now ready to use this script.

Usage
-----

``` sh
./mkvmcore
# creates file "vmcore" with an arbitrary UEK7 osrelease

./mkvmcore mydump
# creates the same, with filename "mydump"

./mkvmcore mydump 6.12.0-4.el9ueknext.x86_64
# like the above, but with a custom osrelease
```

The stdout will give you a command that can be copied & pasted into a shell on a
remote system that will recreate the vmcore file.
