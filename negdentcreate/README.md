negdentcreate
=============

This is a userspace tool for rapidly creating a lot of negative dentries in a
directory quite quickly. It uses predictable numeric names (which can be an
advantage if you want to access the same negative dentry again later, for cache
testing). It supports multithreading, which improves performance up to a point
(in my experience, 4 threads is optimal, and more than that sees diminishing
returns). You can get 100 million negative dentries into a directory in around a
minute or less.

I'd recommend creating a separate directory to contain your negative dentries.
They will get freed when you `rmdir`. Otherwise, you may find the pesky dentries
sticking around for longer than intended, with no means of removal except a
drop_caches.

```

negdentcreate is a tool for creating negative dentries

Currently, it can only create them by calling stat() on files which do
not exist. However, it could be extended to allow creating dentries by
creating files and then deleting them. This tool tries to be performant
by allowing you to tweak the number of threads used. However, know that
more threads is not necessarily better, as the cost of lock contention
may outweigh the gains of parallelism.

Options:
  -t, --threads <N>  use N threads (default 1)
  -c, --count <N>    create N negative dentries (default 1000)
  -p, --path <PATH>  create negative dentries in PATH
  -h, --help         show this message and exit.
```
