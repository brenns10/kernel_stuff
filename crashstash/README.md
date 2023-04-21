crashstash
==========

This is a simple kernel module which creates a file `/proc/crashstash`. The file
is pretty simple, it is not seekable, and it can be read or written, but not
both. Each time you open it in write mode, the previous contents are cleared.
Contents written into it are stored in kernel memory, and can be retrieved by a
read, or you can retrieve them from the vmcore.

Unlike normal files, since the contents are stored in regular kernel pages (i.e.
not page cache), the contents will be included in a vmcore even with
makedumpfile's most stringent dump level, `-d 31`. This means it's a good place
to store some userspace data prior to a crash.
