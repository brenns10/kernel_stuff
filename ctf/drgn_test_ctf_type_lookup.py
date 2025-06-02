#!/usr/bin/env -S drgn -k --no-default-symbols
"""
Verify that variables have types which can be looked up by name in drgn.

CTF has the concept of "non-root types" which cannot be looked up by name. This
should only be used (for the Linux kernel) in cases where there are types of
the same name in the same module. And even if there's a conflict, a name lookup
should return one of the valid conflicting types. This test case iterates
through likely variables and verifies that all the types can be looked up
successfully by name.
"""
import sys

from drgn import TypeKind

try:
    from drgn.helpers.linux.ctf import load_ctf
except (ImportError, ModuleNotFoundError):
    sys.exit("error: drgn is not built with CTF")


load_ctf(prog)
types_checked = set()

total_syms = 0
skipped_syms = 0
non_var_syms = 0
duplicate_type = 0
anon_type = 0
lookup_pass = 0
lookup_syntax = 0
lookup_fail = 0


for line in open("/proc/kallsyms"):
    _, kind, name, *__ = line.split()
    total_syms += 1

    # Skip entries that aren't variables (bss, data, rodata)
    if kind not in ("b", "B", "d", "D", "r", "R"):
        skipped_syms += 1
        continue

    try:
        variable = prog[name]
    except LookupError:
        non_var_syms += 1
        continue

    typename = variable.type_.type_name()
    if typename in types_checked:
        duplicate_type += 1
        continue
    types_checked.add(typename)

    if "<anonymous>" in typename:
        anon_type += 1
        continue

    try:
        prog.type(typename)
        lookup_pass += 1
        print("PASS", typename)
    except SyntaxError:
        lookup_syntax += 1
        print("ERR ", typename)
    except LookupError:
        lookup_fail += 1
        print("FAIL", typename)

print(f"total_syms     = {total_syms}")
print(f"skipped_syms   = {skipped_syms}")
print(f"non_var_syms   = {non_var_syms}")
print(f"duplicate_type = {duplicate_type}")
print(f"anon_type      = {anon_type}")
print(f"lookup_pass    = {lookup_pass}")
print(f"lookup_syntax  = {lookup_syntax}")
print(f"lookup_fail    = {lookup_fail}")
sys.exit("FAIL" if lookup_fail > 0 else 0)
