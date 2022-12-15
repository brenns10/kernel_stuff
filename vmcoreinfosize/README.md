Guess vmcoreinfo note size
==========================

This script constructs a fake vmcoreinfo note data and then measures its length.
Since the vmcoreinfo note is limited to one page, it's important to keep an eye
on whether it may get too long.

The script tries to be conservative, assuming a larger size for most fields. It
also ignores configuration #ifdefs, which would disable some code and reduce the
size of vmcoreinfo. The goal is just to give a warning before we have the
possibility of an overflow.

To run: `python vmcoreinfosize.py path/to/kernel/tree`

(Please note, this file is now a symlink to its new home in the `vmcoreinfo`
directory of this repository, to better situate it with like-minded scripts.
This directory and README are preserved since it is linked from Linux commit
5fd8fea935a1 ("vmcoreinfo: include kallsyms symbols"))
