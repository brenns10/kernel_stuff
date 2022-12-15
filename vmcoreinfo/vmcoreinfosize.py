#!/usr/bin/env python3
"""
Guess the size of vmcoreinfo by using faked data.
"""

import os.path
import re
import sys


ADDR = "f" * 16
NUM = "0000"


def vmcoreinfo_statements(filename, funcname):
    with open(filename) as f:
        lines = list(f)
    stmts = []
    in_fn = False
    for line in lines:
        stripped = line.strip()
        if in_fn:
            if stripped.startswith("VMCOREINFO"):
                stmts.append(stripped)
            elif line == "}\n":
                # return here only
                return stmts
        elif funcname in line:
            in_fn = True
    assert False


def symbol(contents):
    return f"SYMBOL({contents})={ADDR}\n"


def offset(contents):
    struct, field = contents.split(",", 1)
    struct = struct.strip()
    field = field.strip()
    return f"OFFSET({struct}.{field})={NUM}\n"


def number(contents):
    return f"NUMBER({contents})={NUM}\n"


def length(contents):
    name, _ = contents.split(",", 1)
    name = name.strip()
    return f"LENGTH({name})={NUM}\n"


def size(contents):
    return f"SIZE({contents})={NUM}\n"


def pagesize(_):
    # 16k page just  to get that 5 digit number
    return "PAGESIZE=16384\n"


def buildid(_):
    return f"BUILD-ID={40*'f'}\n"


def osrelease(_):
    return f"OSRELEASE={64*'x'}\n"


FUNCTIONS = {
    "SYMBOL": symbol,
    "SYMBOL_ARRAY": symbol,
    "OFFSET": offset,
    "TYPE_OFFSET": offset,
    "NUMBER": number,
    "LENGTH": length,
    "SIZE": size,
    "STRUCT_SIZE": size,
    "PAGESIZE": pagesize,
    "BUILD_ID": buildid,
    "OSRELEASE": osrelease,
}


def main(kernel_tree):
    crashcore = os.path.join(kernel_tree, "kernel/crash_core.c")
    printk = os.path.join(kernel_tree, "kernel/printk/printk.c")
    stmts = (
        vmcoreinfo_statements(crashcore, "crash_save_vmcoreinfo_init")
        + vmcoreinfo_statements(printk, "log_buf_vmcoreinfo_setup")
    )
    fake_vmcoreinfo = ""
    expr = re.compile(r"^VMCOREINFO_([A-Z_]+)\((.*)\);$")
    for stmt in stmts:
        #print(stmt)
        match = expr.match(stmt)
        kind = match.group(1)
        content = match.group(2)
        function = FUNCTIONS[kind]
        fake_vmcoreinfo += function(content)
    print(fake_vmcoreinfo)
    print(f"Length of faked vmcoreinfo: {len(fake_vmcoreinfo)}")


if __name__ == '__main__':
    main(sys.argv[1])
