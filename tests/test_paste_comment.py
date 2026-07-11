# -*- coding: utf-8 -*-
"""Verify paste-as-comment inserts into the EDITOR (undoable, save-gated)."""
import json, os, shutil, sys, tempfile
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import tkinter as tk
import code_annotator as ca

# --- comment rendering per language (unchanged contract) ---
assert ca.comment_lines("hi\nthere", "py") == ["# hi", "# there"]
assert ca.comment_lines("hi", "js") == ["// hi"]
assert ca.comment_lines("hi", "sql") == ["-- hi"]
assert ca.comment_lines("hi", "css") == ["/* hi */"]
assert ca.comment_lines("hi", "html") == ["<!-- hi -->"]
assert ca.comment_lines("hi", "json") is None
assert ca.comment_lines("hi", "plain") is None
print("comment_lines OK")

tmp = tempfile.mkdtemp()
proj = os.path.join(tmp, "proj"); os.makedirs(proj)
src = os.path.join(proj, "sample.py")
body = "import os\n\ndef main():\n    a = 1\n    b = 2\n    return a + b\n"
with open(src, "w", encoding="utf-8", newline="") as f:
    f.write(body)
anndir = os.path.join(proj, ".annotations"); os.makedirs(anndir)
sidecar = os.path.join(anndir, "sample.py.ann.json")
with open(sidecar, "w", encoding="utf-8") as f:
    json.dump({"version": 1, "file": "sample.py", "annotations": [
        {"id": "one", "startLine": 4, "endLine": 5, "color": "green",
         "text": "adds the parts\nline two of note"}]}, f)

root = tk.Tk(); root.geometry("1200x800+3000+3000")
app = ca.CodeAnnotatorApp(root, proj)
root.update()
app.open_file(src); root.update()

# --- default location via Enter: opens the editor, inserts, disk untouched ---
app._start_paste(app.annotations[0])
assert "Enter = line 4" in app.status_hint.cget("text")
assert app._paste_default() == "break"
root.update()
assert app.edit.active, "paste did not open the editor"
text = app.edit.editor.get("1.0", "end-1c")
lines = text.split("\n")
assert lines[3] == "    # adds the parts", lines[3]
assert lines[4] == "    # line two of note", lines[4]
assert lines[5] == "    a = 1"
assert open(src, encoding="utf-8").read() == body, "disk changed before save"
assert app.edit.dirty, "pending edit not marked dirty"
assert "Ctrl+Z undoes" in app.status_hint.cget("text")
print("editor insertion at default location OK (disk untouched)")

# --- single Ctrl+Z removes the whole comment block; redo restores it ---
app.edit.editor.edit_undo(); root.update()
assert app.edit.editor.get("1.0", "end-1c") == body, "undo did not remove comment"
app.edit.editor.edit_redo(); root.update()
assert app.edit.editor.get("1.0", "end-1c") == text, "redo did not restore comment"
print("undo/redo of pasted comment OK")

# --- save: disk updated, annotation re-anchored via its band tag ---
app.edit.save(); root.update()
on_disk = open(src, encoding="utf-8").read()
assert on_disk == text + "\n" or on_disk == text, on_disk
ann = json.load(open(sidecar, encoding="utf-8"))["annotations"][0]
assert (ann["startLine"], ann["endLine"]) == (6, 7), ann
app.edit.toggle(); root.update()
assert not app.edit.active and app.annotations[0]["startLine"] == 6
print("save + annotation re-anchor OK")

# --- custom location via gutter click, then discard instead of saving ---
before = open(src, encoding="utf-8").read()
app._start_paste(app.annotations[0])
class Ev: y = app.pad + 2                     # a point inside line 1
app._gutter_press(Ev); root.update()
assert app.edit.active
assert app.edit.editor.get("1.0", "2.0").startswith("# adds the parts")
import tkinter.messagebox as mb
mb.askyesnocancel = lambda *a, **k: False     # discard on exit
app.edit.toggle(); root.update()
assert not app.edit.active
assert open(src, encoding="utf-8").read() == before, "discard leaked to disk"
print("custom-location insert + discard OK")

# --- Escape cancels placement without opening the editor ---
app._start_paste(app.annotations[0])
app._close_bars()
assert app._paste_ann is None and not app.edit.active
print("escape cancel OK")

# --- no comment syntax: no editor, no changes ---
app._paste_ann = dict(app.annotations[0])
app.lang_id = "json"
app._paste_comment_at(1)
assert not app.edit.active
app.lang_id = "py"
print("no-comment-syntax no-op OK")

root.destroy(); shutil.rmtree(tmp)
print("ALL PASTE-AS-COMMENT CHECKS PASSED")
