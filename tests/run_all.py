# -*- coding: utf-8 -*-
"""Run every test_*.py in this folder, each in its own interpreter.

The GUI tests drive the real app in Tk windows positioned off-screen, so a
display is required, but nothing pops up over your work. Exit status is
nonzero if any test file fails.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"   # emoji in test output on Windows
    names = sorted(n for n in os.listdir(HERE)
                   if n.startswith("test_") and n.endswith(".py"))
    failed = []
    for name in names:
        print("== %s ==" % name)
        sys.stdout.flush()              # keep headers ordered with child output
        code = subprocess.call([sys.executable, os.path.join(HERE, name)],
                               env=env)
        if code != 0:
            failed.append(name)
    print()
    if failed:
        print("FAILED: %s" % ", ".join(failed))
        return 1
    print("all %d test files passed" % len(names))
    return 0


if __name__ == "__main__":
    sys.exit(main())
