obj-m += ftrace_killer.o

CFLAGS_ftrace_killer.o := $(shell sudo grep \\bftrace_kill$$ /proc/kallsyms | awk '{ printf("-DEXTERN_FTRACE_KILL=0x%s\n", $$1); }')

all:
	@# Forces the kernel module to be rebuilt every time
	touch ftrace_killer.c
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
