#!/usr/bin/env python3
"""
Given a vmcore or live system, and the address of a "struct assoc_array", this
script will write a file (construct_array.c) which contains the necessary C
statements to construct an equivalent array in userspace.

Usage:
    drgn -c vmcore -s vmlinux.debug assoc_array_constructor.py $ADDRESS
"""
import sys
import typing as t

import drgn


def assoc_array_ptr_to_node(ptr: drgn.Object):
    ptr_long = drgn.cast("unsigned long", ptr)
    assert ptr_long & 0x1 and not (ptr_long & 0x2)
    return drgn.cast("struct assoc_array_node *", ptr_long - 1)


def assoc_array_ptr_is_node(ptr: drgn.Object):
    ptr_long = drgn.cast("unsigned long", ptr)
    return (ptr_long & 0x1 and not (ptr_long & 0x2))


def assoc_array_ptr_is_shortcut(ptr: drgn.Object):
    ptr_long = drgn.cast("unsigned long", ptr)
    return (ptr_long & 0x3) == 0x3


def print_node(node: drgn.Object, level: int, index: int):
    fanout = 16  # ASSOC_ARRAY_FAN_OUT
    node_long = drgn.cast("unsigned long", node)
    if node_long & 0x1:
        if node_long & 0x2:
            print(f"{'   ' * level}[{index:x}]: SHORTCUT - NOT IMPLEMENTED")
        else:
            node = drgn.cast("struct assoc_array_node *", node_long - 1)
            print(f"{'   ' * level}[{index:x}]: NODE {node_long.value_() - 1:x}")
            for i in range(fanout):
                ptr = node.slots[i]
                if ptr:
                    print_node(node.slots[i], level + 1, i)
    else:
        print(f"{'   ' * level}[{index:x}]: LEAF OBJ: {node_long.value_():x}")


def print_assoc_array(arr: drgn.Object):
    print_node(arr.root, 0, 0)


def construct_node(node: drgn.Object, stmts: t.List[str]):
    """
    This function creates a bunch of C statements which create an associative
    array. Uses the following helpers which you can find in repro.c:
    - struct assoc_array_node *node - our one variable to use
    - mknode() - allocates a zero'd node
    - set_data() - sets the parent_slot and nr_leaves_on_branch values
    - set_node() - sets a node pointer
    - set_leaf() - sets a leaf pointer
    - get_node() - returns the node pointer at an index
    - get_parent() - returns the parent node pointer
    """
    fanout = 16
    ps = node.parent_slot.value_()
    nb = node.nr_leaves_on_branch.value_()
    stmts.append(f"set_data(node, {ps}, 0x{nb:x}UL);")
    for i in range(fanout):
        ptr = node.slots[i]
        ptr_long = drgn.cast("unsigned long", ptr).value_()
        assert not assoc_array_ptr_is_shortcut(ptr)
        if assoc_array_ptr_is_node(ptr):
            stmts.append(f"set_node(node, {i});")
            stmts.append(f"node = get_node(node, {i});")
            construct_node(assoc_array_ptr_to_node(ptr), stmts)
            stmts.append("node = get_parent(node);")
        elif ptr_long:
            stmts.append(f"set_leaf(node, {i}, 0x{ptr_long:x}UL);")


def construct_array(array: drgn.Object) -> str:
    nt = array.nr_leaves_on_tree.value_()
    node = assoc_array_ptr_to_node(array.root)  # asserts
    stmts = [
        f"array->nr_leaves_on_tree = {nt}UL;",
        "node = mknode();",
        "array->root = assoc_array_node_to_ptr(node);",
    ]
    construct_node(node, stmts)
    return "\n".join("\t" + line for line in stmts)


if len(sys.argv) != 2:
    print("usage: drgn -c vmcore -s vmlinux.debug assoc_array_constructor.py $ADDRESS")
    sys.exit(1)
try:
    addr = int(sys.argv[1], 16)
except:
    print("error: Address must be a hexadecimal literal")
    sys.exit(1)
array = drgn.Object(prog, "struct assoc_array *", addr)
arr_txt = construct_array(array)
with open("construct_array.c", "w") as f:
    f.write(arr_txt)
print("Array constructor code is in construct_array.c!")
