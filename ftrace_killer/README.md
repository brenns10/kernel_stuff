ftrace_killer
=============

Kernel module to call `ftrace_kill()` simulating a ftrace error which triggered
a `FTRACE_WARN_ON`, killing the ftrace subsystem. `ftrace_kill()` is not
exported so we circumvent that by grepping its address from kallsyms and using
it as a function pointer in the code.

It needs to be rebuilt each time you reboot, if KASLR is enabled.

``` sh
make
sudo insmod ./ftrace_killer.ko
# ftrace is now dead, RIP
```

https://lore.kernel.org/all/20240501162956.229427-1-stephen.s.brennan@oracle.com/
