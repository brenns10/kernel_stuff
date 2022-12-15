#!/usr/bin/env python3
"""
Use strings to find vmcoreinfo. This is to be used when the vmcore is not
properly created, and so the vmcoreinfo note is not present in the expected
place. We need to do a search over a very large haystack. See my tool
get_vmcoreinfo for the simpler (and faster) approach for normal vmcores.
"""
import argparse
import subprocess
import sys


def yield_blocks(filp, sep, bs=4096):
    buf = bytearray()
    while True:
        data = filp.read(bs)
        # Check for 0 data written, that's EOF
        if len(data) == 0:
            if buf:
                yield bytes(buf)
            return
        buf.extend(data)
        while True:
            ix = buf.find(sep)
            if ix < 0:
                break
            yield bytes(buf[:ix])
            del buf[:ix + len(sep)]


def main(args):
    print("Hunting!")
    vmcore = sys.argv[1]
    proc = subprocess.Popen(
        ["strings", "--output-separator=||||", "-w", "-n", "128", vmcore],
        stdout=subprocess.PIPE,
    )

    for string in yield_blocks(proc.stdout, b"||||"):
        if b"OSRELEASE=" in string and b"PAGESIZE=" in string and b"SYMBOL(" in string:
            with open(args.output, "wb") as f:
                f.write(string)
            print(f"\nVMCOREINFO FOUND!\nWrote to {args.output}")
            break
        print(".", end="")
        sys.stdout.flush()
    else:
        print("Terminated without finding vmcoreinfo, sorry!")

    proc.terminate()


if __name__ == '__main__':
    p = argparse.ArgumentParser(description="find vmcoreinfo section of file")
    p.add_argument("vmcore", help="vmcore file to search")
    p.add_argument("output", help="output file to write")
    a = p.parse_args()
    main(a)
