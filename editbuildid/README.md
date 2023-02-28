Edit ELF Build-ID
=================

This quick program gives you the ability to edit the Build ID value of an
already built ELF file. Build IDs are easy to edit, because they are typically a
fixed size (most commonly 20 bytes, but 16 byte IDs also exist), so we don't
need to shift any other data in the file around, nor do we need to update any
offsets. The tool doesn't support changing the size of a build ID (for example,
you can edit a 20-byte ID to a different 20-byte value, but not to any other
size).

```
usage: editbuildid [-n BUILD-ID] [-p] [-v] [-h] ELF-FILE

Find the build ID of an ELF file and either print it (-p) and exit, or
overwrite it with the given value (-n BUILD-ID). The -p and -n options
are mutually exclusive and exactly one must be specified.

Options:
  -n, --new BUILD-ID   specify the new BUILD-ID value
  -p, --print          print the current build ID value and exit
  -v, --verbose        print informational messages
  -h, --help           print this message and exit
```


Why?
----

I have occasionally encountered issues where a debuginfo file did not have a
matching build ID. If this happens, it is obviously a packaging bug, but that
doesn't solve the problem at hand! To "force" debuggers to use that debuginfo
file, you can simply edit it to match!

Obviously, if the debuginfo doesn't closely match your debugged program, you'll
probably still end up crashing your debugger. But you already voided your
warranty when you decided to edit the ELF file :)
