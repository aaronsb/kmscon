"""A renderer module links its own copy of the render library, so every call
it makes into process state must bind to kmscon's copy: each function of
PREFIX that SOURCE calls has to be in the dynamic symbol table of both the
executable and the module, where the dynamic linker resolves the module's call
to the executable's definition. A hidden one stays the module's own, with
state of its own (#188: gltex counted its display and its textures in a
budget kmscon never read).

Usage: check_module_symbols.py EXECUTABLE MODULE SOURCE PREFIX
"""

import re
import subprocess
import sys


def dynamic(path):
    out = subprocess.run(["nm", "-D", "--defined-only", path], capture_output=True, text=True,
                         check=True).stdout
    return {line.split()[-1] for line in out.splitlines() if line.strip()}


def main():
    exe, module, source, prefix = sys.argv[1:5]
    with open(source) as f:
        called = sorted(set(re.findall(r"\b(%s\w+)\s*\(" % re.escape(prefix), f.read())))
    if not called:
        print("%s calls nothing named %s*: the check would check nothing" % (source, prefix))
        return 1
    in_exe, in_module = dynamic(exe), dynamic(module)
    bad = [n for n in called if n not in in_exe or n not in in_module]
    for n in called:
        print("%-36s kmscon %-3s module %s" % (n, "yes" if n in in_exe else "NO",
                                                "yes" if n in in_module else "NO"))
    if bad:
        print("not exported, so the module keeps its own copy: %s" % ", ".join(bad))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
