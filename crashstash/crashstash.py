#!/usr/bin/env python3
import math
import re

from drgn import Object
from drgn import Program
from drgn.helpers.linux.list import list_for_each_entry
from drgn.helpers.linux.printk import get_printk_records
from drgn.helpers.linux.mm import page_to_virt


def get_crashstash(prog: Program) -> bytes:
    log = get_printk_records(prog)
    log.sort(key=lambda l: l.timestamp)
    expr = re.compile(
        rb"crashstash: STASH: (?P<STASH>[a-fA-F0-9]+) "
        rb"SIZE: (?P<SIZE>[a-fA-F0-9]+) "
        rb"PAGES: (?P<PAGES>[a-fA-F0-9]+)"
    )
    for rec in reversed(log):
        match = expr.fullmatch(rec.text)
        if match:
            break
    else:
        raise Exception("Could not find a crashstash log record")

    head = Object(prog, "struct list_head *", value=int(match.group("STASH"), 16))
    size = Object(prog, "u64", address=int(match.group("SIZE"), 16)).value_()
    page_count = Object(prog, "u64", address=int(match.group("PAGES"), 16)).value_()

    pages = list(list_for_each_entry("struct page", head, "lru"))
    PAGE_SIZE = prog["PAGE_SIZE"].value_()

    if len(pages) != page_count or page_count != math.ceil(size / PAGE_SIZE):
        raise Exception("Inconsistent metadata for crashstash size")

    data = b""
    for page in pages:
        chunksz = min(PAGE_SIZE, size)
        chunkdat = prog.read(page_to_virt(page), chunksz)
        data += chunkdat
        size -= chunksz
    return data
