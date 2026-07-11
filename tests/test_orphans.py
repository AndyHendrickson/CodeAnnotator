# -*- coding: utf-8 -*-
"""Verify orphaned-annotation handling."""
import json, os, shutil, sys, tempfile
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import tkinter as tk
import code_annotator as ca

tmp = tempfile.mkdtemp()
proj = os.path.join(tmp, "proj"); os.makedirs(proj)
anndir = os.path.join(proj, ".annotations"); os.makedirs(anndir)
src = os.path.join(proj, "sample.py")
body = "import os\n\ndef main():\n    a = 1\n    b = 2\n    return a + b\n"

def reset(orphaned=False):
    with open(src, "w", encoding="utf-8") as f:
        f.write(body)
    ann = {"id": "one", "startLine": 4, "endLine": 5, "color": "green",
           "text": "the locals"}
    if orphaned:
        ann["orphaned"] = True
    with open(os.path.join(anndir, "sample.py.ann.json"), "w",
              encoding="utf-8") as f:
        json.dump({"version": 1, "file": "sample.py", "annotations": [ann]}, f)

def sidecar():
    return json.load(open(os.path.join(anndir, "sample.py.ann.json"),
                          encoding="utf-8"))["annotations"][0]

root = tk.Tk(); root.geometry("1200x800+3000+3000")
reset()
app = ca.CodeAnnotatorApp(root, proj)
root.update()

# --- deleting the whole annotated region orphans the annotation ---
app.open_file(src); root.update()
app.edit.toggle(); root.update()
app.edit.editor.delete("3.0", "7.0")          # wipes lines 3-6 incl. the region
root.update()
app.edit.save(); root.update()
a = sidecar()
assert a.get("orphaned") is True, a
assert a["startLine"] == a["endLine"] == 3, a
app.edit.toggle(); root.update()
assert app.annotations[0]["orphaned"] is True
print("full deletion -> orphaned at the deletion point OK")

# --- viewer renders it distinctly: dashed edge + warning note prefix ---
note = [i for i in app.canvas.find_withtag("note")
        if "ann_one" in app.canvas.gettags(i)]
assert note and app.canvas.itemcget(note[0], "text").startswith("⚠ orphaned"), \
    app.canvas.itemcget(note[0], "text") if note else "no note"
dashed = [i for i in app.canvas.find_withtag("ann_one")
          if app.canvas.type(i) == "line"]
assert dashed, "no dashed orphan edge drawn"
print("orphaned rendering (warning note + dashed edge) OK")

# --- re-saving from the dialog clears the flag ---
def drive():
    for w in root.winfo_children():
        if isinstance(w, ca.AnnotationDialog):
            w._save()
root.after(200, drive)
app._edit_annotation(app.annotations[0], is_new=False, x=200, y=200)
root.update()
assert app.annotations[0]["orphaned"] is False
raw = open(os.path.join(anndir, "sample.py.ann.json"), encoding="utf-8").read()
assert "orphaned" not in raw, "cleared flag still written to disk"
print("dialog re-save clears the orphan flag OK")

# --- partial deletion does NOT orphan (band tag survives) ---
reset()
app.open_file(src); root.update()
app.edit.toggle(); root.update()
app.edit.editor.delete("4.0", "5.0")          # remove only line 4 of region 4-5
root.update()
app.edit.save(); root.update()
a = sidecar()
assert not a.get("orphaned"), a
assert (a["startLine"], a["endLine"]) == (4, 4), a
app.edit.toggle(); root.update()
print("partial deletion re-anchors normally, not orphaned OK")

# --- an already-orphaned annotation survives an edit session untouched ---
reset(orphaned=True)
app.open_file(src); root.update()
assert app.annotations[0]["orphaned"] is True
app.edit.toggle(); root.update()
app.edit.editor.insert("end", "x = 9\n")      # unrelated edit elsewhere
root.update()
app.edit.save(); root.update()
a = sidecar()
assert a.get("orphaned") is True and (a["startLine"], a["endLine"]) == (4, 5), a
app.edit.toggle(); root.update()
print("existing orphan flag preserved through unrelated edits OK")

root.destroy(); shutil.rmtree(tmp)
print("ALL ORPHANED-ANNOTATION CHECKS PASSED")
