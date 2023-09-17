libcore2
========

The concept of libcore2 is that it glues together:

1. libkdumpfile which can read data out of vmcores and `/proc/kcore`
2. libctf which can give us structure offsets (TODO)
3. kallsyms which can give us symbols (with recent linux patches, kallsyms can
   be parsed from vmcores even if it's not a live system)

The result would be a program that can run live or against dumps, and do very
basic data extraction. However, the approach is super limited, and is intended
to stay that way:

- It only targets the host architecture. No support for reading vmcores from
  other architectures. Also for now, it's only targeting x86_64.
- It only targets "well-formed" vmcores: i.e. those from kdump/makedumpfile, or
  just the standard `/proc/vmcore` and `/proc/kcore`. There's no hope to handle
  weird Qemu or Xen vmcores, let alone other bespoke formats.
- It is not intended to be fully generic, targeting any kernel version. Programs
  will likely have some compiled-in assumptions about the types (e.g. integer
  type sizes would likely be hard-coded), with some flexibility for variations
  (e.g.  structure offsets). There's always a trade-off between making a program
  generic and writing a program for a specific kernel version, and this
  framework hopes to be useful across that spectrum.

The idea for this is that it could be useful if you want to run some Drgn helper
script, but you need it to be faster, or you need it to be run in very
constrained, low-memory situations (like the kdump environment). You could port
it here, accept some of the trade-offs, and use it in those limited situations.

Features
--------

- [x] lookup symbol (live)
- [ ] lookup symbol (core dump) - not implemented, need to add vmcoreinfo
      support
- [ ] get structure offset (libctf) - not implemented
- [x] helpers for reading types: (`read_u64`, `read_u32`, etc). Partial
      implementation.
- [ ] dentrycache implementation

Some interesting stretch goals:

- [ ] compile-time libctf support for structure offsets via a table (no change
      in API)
- [ ] assertions about various types (e.g. `saved_command_line_len` is a 4-byte
      integer). This would allow code to run without needing to be made generic,
      yet allow it to detect when its assumptions are violated. Would be good
      for this to work compile-time too.
- [ ] add efficient tree data structures for caching

