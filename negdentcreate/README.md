negdentcreate
=============

```
negdentcreate is a tool for creating negative dentries

It creates predictable numeric filenames and divides them up among many
threads, where each thread does an 'operation'. By default, they simply
run stat(filename), which results in the creation of a negative dentry.
In addition to stat(), an alternative is to create and unlink a file,
which also creates a negative dentry, but it is much slower since file
creation usually involves filesystem I/O. Finally, negdentcreate also
supports operations like 'open' or 'create' or 'unlink', and it can run
these operations in an endless loop, to generate silly filesystem load.

Options:
  -t, --threads <N>  distribute work across N threads (default 1)
  -c, --count <N>    create N filenames (default 1000)
  -p, --path <PATH>  create negative dentries / files in PATH
  -P, --pfx  <STR>   prefix for filenames. Each file is named with this
                     prefix followed by a zero-based index. The default
                     is "file-"), resulting in: file-0000000000
  -o, --op <STR>     operation (choices: stat, open, create, unlink,
                     create_unlink_close, or create_close_unlink)
  -l, --loop         loop continuously
  -h, --help         show this message and exit.
```

