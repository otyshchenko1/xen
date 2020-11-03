#!/usr/bin/env python
import sys

policy_size = 0

sys.stdout.write("""
/* This file is autogenerated by gen_policy.py */
#include <xen/init.h>
#include <xsm/xsm.h>

const unsigned char xsm_flask_init_policy[] __initconst = {
""")

for char in sys.stdin.read():
    sys.stdout.write(" 0x%02x," % ord(char))
    policy_size = policy_size + 1
    if policy_size % 13 == 0:
        sys.stdout.write("\n")

sys.stdout.write("""
};
const unsigned int __initconst xsm_flask_init_policy_size = %d;
""" % policy_size)

sys.stdout.write("""
void policy_dummy_func(void) {}
""")
