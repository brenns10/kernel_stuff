Edit ELF Build-ID
=================

This quick program gives you the ability to edit the Build ID value of an
already built ELF file. It's only written for 64-bit x86_64 but it probably
could be expanded quite easily.

Build IDs are easy to edit, because they are always 20 bytes, so we don't need
to shift any other data in the file around, nor do we need to update any
offsets.

Use it like so:

``` bash
$ gcc -o editbuildid{,.c}
$ ./editbuildid foo.ko DEADBEEFBEEFBEEFBEEFBEEFBEEFBEEFBEEFBEEF
ELF file has no program header
Found NOTES section in section header index 1
Found old build ID: 29cc2c2355a2a248d48e3d3f1508f0b7b5abb21c
Wrote new build ID: DEADBEEFBEEFBEEFBEEFBEEFBEEFBEEFBEEFBEEF
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
