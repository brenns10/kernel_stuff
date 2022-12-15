editvmcoreinfo
==============

This directory contains tools for wrangling ELF vmcores.

findvmcoreinfo.py is a tool for finding the VMCOREINFO file out of any
uncompressed binary using strings.

editvmcoreinfo.c is a tool for taking an ELF vmcore which has bad VMCOREINFO
data, and replacing it with the real data found in findvmcoreinfo.py.

You'll need libelf headers (I think elfutils-devel is your OL package) but I
don't actually link to libelf because it's sorta... useless for this. I just
use their struct declarations.
