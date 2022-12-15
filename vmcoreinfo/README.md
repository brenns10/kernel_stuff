vmcoreinfo
==========

This directory contains tools related to vmcoreinfo!

`findvmcoreinfo.py`
-------------------

`findvmcoreinfo.py` is a small script on top of `strings` which searches a large
binary file for a string which looks like the vmcoreinfo note. I wrote it when I
had a hypervisor core dump, but the vmcoreinfo note it contained was completely
fabricated. This tool can be used to find the real vmcoreinfo note and write it
to a text file.

Note that since it is just a glorified wrapper around strings, it won't work on
vmcores which are compressed. Mainly, it would work on ELF dumps, or possibly
uncompressed makedumpfile formats.

`editvmcoreinfo.c`
------------------

`editvmcoreinfo.c` is a tool for taking an ELF vmcore which has bad vmcoreinfo
data, and replacing it with the real data (for example, the output of
`findvmcoreinfo.py`).

To compile it, you'll need libelf headers (I think elfutils-devel is your OL
package) but I don't actually link to libelf because it's sorta... useless for
this. I just use the structure definitions from the headers.

``` sh
gcc -o editvmcoreinfo editvmcoreinfo.c
```

`get_vmcoreinfo.c`
------------------

`get_vmcoreinfo.c` is a tool that can take a vmcore in makedumpfile/kdump
format, and output its vmcoreinfo. It can also do this for an ELF format vmcore,
but if you have one of those, you can just use `readelf -n`.

This one needs to link to `libkdumpfile`, you'll need that installed either via
your package manager, or from source. Then:

``` sh
gcc -o get_vmcoreinfo get_vmcoreinfo.c -lkdumpfile
```

`vmcoreinfosize.py`
-------------------

This is a tool for guestimating the size of the vmcoreinfo note in a Linux
kernel source tree.

It tries to guess a safe upper bound to each type of field, and adds them up.
The goal is to get an idea whether it is safe to add a new field.
