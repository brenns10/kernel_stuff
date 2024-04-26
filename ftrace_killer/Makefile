obj-m += ftrace_killer.o

CFLAGS_ftrace_killer.o := $(shell sudo grep ftrace_kill /proc/kallsyms | awk '{ printf("-DEXTERN_FTRACE_KILL=0x%s\n", $$1); }')

# force rebuilding every time
.PHONY: ftrace_killer.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
