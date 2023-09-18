LDLIBS := -lkdumpfile -lctf
CFLAGS := -g

.PHONY: all
all: cmdline

cmdline: libcore.o cmdline.o

libcore.o: libcore.c libcore.h
cmdline.o: cmdline.c libcore.h

.PHONY: clean
clean:
	rm -f libcore.o cmdline.o cmdline
