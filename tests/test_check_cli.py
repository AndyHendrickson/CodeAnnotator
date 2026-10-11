# -*- coding: utf-8 -*-
# Copyright (c) 2026 Andy Hendrickson
# SPDX-License-Identifier: MIT
"""The --check / --fix command-line mode."""
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "code_annotator.py")

tmp = tempfile.mkdtemp()
proj = os.path.join(tmp, "proj"); os.makedirs(proj)
anndir = os.path.join(proj, ".annotations"); os.makedirs(anndir)


def run_check(*extra):
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    result = subprocess.run([sys.executable, SCRIPT, proj, "--check"] +
                            list(extra), capture_output=True, text=True,
                            env=env)
    return result.returncode, result.stdout


def write(path, text):
    full = os.path.join(proj, path)
    os.makedirs(os.path.dirname(full), exist_ok=True) if os.sep in path else None
    with open(full, "w", encoding="utf-8") as f:
        f.write(text)


def write_ann(name, anns):
    with open(os.path.join(anndir, name + ".ann.json"), "w",
              encoding="utf-8") as f:
        json.dump({"version": 1, "file": name, "annotations": anns}, f)


# clean project -> exit 0
write("good.py", "a = 1\nb = 2\nc = 3\n")
write_ann("good.py", [{"id": "g", "startLine": 2, "endLine": 2,
                       "color": "blue", "text": "ok", "anchor": "b = 2"}])
code, out = run_check()
assert code == 0 and "0 problem(s)" in out, out
print("clean check OK")

# drifted anchor -> exit 1 with WARN; --fix repairs; re-check passes
write("good.py", "# pad\n# pad\na = 1\nb = 2\nc = 3\n")
code, out = run_check()
assert code == 1 and "anchor drifted" in out, out
code, out = run_check("--fix")
assert code == 0 and "FIXED" in out and "1 re-anchored" in out, out
code, out = run_check()
assert code == 0, out
fixed = json.load(open(os.path.join(anndir, "good.py.ann.json"),
                       encoding="utf-8"))["annotations"][0]
assert fixed["startLine"] == 4, fixed
print("drift detection + --fix OK")

# out-of-range annotation -> reported
write_ann("good.py", [{"id": "g", "startLine": 99, "endLine": 120,
                       "color": "blue", "text": "beyond"}])
code, out = run_check()
assert code == 1 and "beyond" in out, out
print("out-of-range detection OK")

# corrupt sidecar -> error even with --fix
write_ann("good.py", [{"id": "g", "startLine": 2, "endLine": 2,
                       "color": "blue", "text": "ok", "anchor": "b = 2"}])
with open(os.path.join(anndir, "bad.py.ann.json"), "w") as f:
    f.write("{ nope")
write("bad.py", "x = 1\n")
code, out = run_check("--fix")
assert code == 1 and "unparseable" in out, out
os.remove(os.path.join(anndir, "bad.py.ann.json"))

# sidecar whose source is gone -> error
write_ann("ghost.py", [{"id": "x", "startLine": 1, "endLine": 1,
                        "color": "pink", "text": "hi"}])
code, out = run_check()
assert code == 1 and "source file is missing" in out, out
print("corrupt + missing-source detection OK")

shutil.rmtree(tmp)
print("ALL CHECK-CLI CHECKS PASSED")
