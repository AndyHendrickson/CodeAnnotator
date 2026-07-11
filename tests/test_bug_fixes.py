# -*- coding: utf-8 -*-
"""Verify the five review-bug fixes."""
import json, os, shutil, sys, tempfile
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import tkinter as tk
import tkinter.messagebox as mb
import code_annotator as ca

tmp = tempfile.mkdtemp()
proj = os.path.join(tmp, "proj"); os.makedirs(proj)
anndir = os.path.join(proj, ".annotations"); os.makedirs(anndir)

def write(name, text):
    p = os.path.join(proj, name)
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)
    return p

def sidecar(name, payload, raw=None):
    p = os.path.join(anndir, name + ".ann.json")
    with open(p, "w", encoding="utf-8") as f:
        f.write(raw if raw is not None else json.dumps(payload))
    return p

# fixture files
blank = write("blank.py", "\n\n\n\n")
sidecar("blank.py", {"version": 1, "file": "blank.py", "annotations": [
    {"id": "x", "startLine": 2, "endLine": 3, "color": "blue", "text": "hi"}]})
plain = write("plain.py", "a = 1\nb = 2\nc = 3\n")
corrupt = write("corrupt.py", "x = 1\n")
corrupt_sc = sidecar("corrupt.py", None, raw="{ this is not json !!")
weird = write("a ●.py", "z = 1\n")
sidecar("a ●.py", {"version": 1, "file": "a ●.py", "annotations": [
    {"id": "w", "startLine": 1, "endLine": 1, "color": "pink", "text": "n"}]})

root = tk.Tk(); root.geometry("1200x800+3000+3000")
app = ca.CodeAnnotatorApp(root, proj)
root.update()

# 1) crash fix: annotated all-blank file opens; z-order raise with find active
app.open_file(blank); root.update()
assert app.annotations and app.canvas.find_withtag("ann")
app.open_file(plain); root.update()          # no annotations at all
app.finder.open_find(); app.finder.find_entry.insert(0, "a"); app.finder.refresh()
app.draw_annotations()                       # used to tag_raise over empty 'ann'
app._close_bars()
print("1) blank-file + empty-ann z-order crash fixed")

# 2) corrupt sidecar: load refuses, saving locked, file preserved
raised = False
try:
    ca.load_annotations(corrupt_sc)
except ca.AnnotationLoadError:
    raised = True
assert raised, "loader did not raise on corrupt file"
app.open_file(corrupt); root.update()
assert app._ann_error and app.annotations == []
assert "unreadable" in app.status_hint.cget("text")
errors = []
mb.showerror = lambda *a, **k: errors.append(a)
app.annotations = [{"id": "n1", "startLine": 1, "endLine": 1,
                    "color": "blue", "text": "new"}]
app._persist()
assert errors, "persist did not refuse"
assert open(corrupt_sc, encoding="utf-8").read() == "{ this is not json !!", \
    "corrupt sidecar was overwritten"
class Ev: y = 10
app._gutter_press(Ev)                        # annotating blocked with a flash
assert app._drag_start is None and "disabled" in app.status_hint.cget("text")
print("2) corrupt sidecar preserved; saving and annotating locked")

# 3) rstrip badge fix: filename ending in ' ●' survives refresh
app.open_file(weird); root.update()
item = app.tree_pane.item_by_path[weird]
before = app.tree_pane.tree.item(item, "text")
for _ in range(3):
    app.tree_pane.refresh_badge(weird)
assert app.tree_pane.tree.item(item, "text") == before, (before,
                                               app.tree_pane.tree.item(item, "text"))
assert "a ●.py" in before
print("3) badge refresh preserves names ending in the badge character")

# 4) flash timer: a newer flash is not cleared by an older timer
app.open_file(plain); root.update()
app._flash_status("first", ms=150)
app._flash_status("second", ms=4000)
root.after(400, root.quit); root.mainloop()  # let the 150ms timer (if any) fire
assert app.status_hint.cget("text") == "second", app.status_hint.cget("text")
print("4) overlapping flash timers fixed")

# 5) atomic saves: content correct, no temp droppings
app.edit.toggle(); root.update()
assert app.edit.active
app.edit.editor.insert("end", "d = 4\n")
app.edit.save(); root.update()
assert open(plain, encoding="utf-8").read().endswith("d = 4\n")
assert not [f for f in os.listdir(proj) if f.endswith(".tmp~")], "temp left"
app.edit.toggle(); root.update()
app.annotations = [{"id": "n2", "startLine": 1, "endLine": 1,
                    "color": "blue", "text": "ok"}]
app._ann_error = False
app._persist()
assert not [f for f in os.listdir(anndir) if f.endswith(".tmp~")]
loaded = ca.load_annotations(os.path.join(anndir, "plain.py.ann.json"))
assert loaded[0]["text"] == "ok"
print("5) atomic source + annotation saves OK, no temp files left")

root.destroy(); shutil.rmtree(tmp)
print("ALL BUG-FIX CHECKS PASSED")
