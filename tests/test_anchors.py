# -*- coding: utf-8 -*-
# Copyright (c) 2026 Andy Hendrickson
# SPDX-License-Identifier: MIT
"""Anchor-based self-healing when files change outside the app."""
import json
import os
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import tkinter as tk
import code_annotator as ca

tmp = tempfile.mkdtemp()
proj = os.path.join(tmp, "proj"); os.makedirs(proj)
anndir = os.path.join(proj, ".annotations"); os.makedirs(anndir)
src = os.path.join(proj, "sample.py")
sidecar = os.path.join(anndir, "sample.py.ann.json")
body = "import os\n\ndef main():\n    a = 1\n    b = 2\n    return a + b\n"


def write_src(text):
    with open(src, "w", encoding="utf-8") as f:
        f.write(text)


def write_ann(ann):
    with open(sidecar, "w", encoding="utf-8") as f:
        json.dump({"version": 1, "file": "sample.py",
                   "annotations": [ann]}, f)


root = tk.Tk(); root.geometry("1200x800+3000+3000")
write_src(body)
write_ann({"id": "one", "startLine": 4, "endLine": 5, "color": "green",
           "text": "the locals", "anchor": "a = 1"})
app = ca.CodeAnnotatorApp(root, proj)
root.update()

# --- drifted file: annotation follows its anchor and the fix is persisted ---
write_src("# new header\n# more header\n" + body)   # everything shifts by 2
app.open_file(src); root.update()
a = app.annotations[0]
assert (a["startLine"], a["endLine"]) == (6, 7), a
assert "re-anchored" in app.status_hint.cget("text")
on_disk = json.load(open(sidecar, encoding="utf-8"))["annotations"][0]
assert (on_disk["startLine"], on_disk["endLine"]) == (6, 7), on_disk
print("drift repaired via anchor and persisted OK")

# --- anchored code deleted entirely: annotation becomes orphaned ---
write_src("import os\n\nprint('rewritten')\n")
app.open_file(src); root.update()
a = app.annotations[0]
assert a["orphaned"] is True, a
assert "orphaned" in app.status_hint.cget("text")
print("missing anchor -> orphaned OK")

# --- legacy sidecar without anchors: adopted in memory, written on save ---
write_src(body)
write_ann({"id": "two", "startLine": 4, "endLine": 5, "color": "blue",
           "text": "legacy note"})
app.open_file(src); root.update()
assert app.annotations[0]["id"] == "two"
assert app.annotations[0]["anchor"] == "a = 1"       # adopted
raw = open(sidecar, encoding="utf-8").read()
assert '"anchor"' not in raw, "adoption alone must not rewrite the file"
app._persist()
raw = open(sidecar, encoding="utf-8").read()
assert '"anchor": "a = 1"' in raw
print("anchor adoption on legacy sidecars OK")

root.destroy(); shutil.rmtree(tmp)
print("ALL ANCHOR CHECKS PASSED")
