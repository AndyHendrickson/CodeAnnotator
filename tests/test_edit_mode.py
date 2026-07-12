# -*- coding: utf-8 -*-
"""Verify edit mode: editing, undo/redo, save, annotation re-anchoring."""
import json, os, shutil, sys, tempfile
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import tkinter as tk
import tkinter.messagebox as mb
import code_annotator as ca

tmp = tempfile.mkdtemp()
proj = os.path.join(tmp, "proj")
os.makedirs(proj)
src = os.path.join(proj, "sample.py")
body = "import os\n\ndef main():\n    a = 1\n    b = 2\n    return a + b\n"
with open(src, "w", encoding="utf-8", newline="") as f:
    f.write(body)
anndir = os.path.join(proj, ".annotations")
os.makedirs(anndir)
with open(os.path.join(anndir, "sample.py.ann.json"), "w", encoding="utf-8") as f:
    json.dump({"version": 1, "file": "sample.py", "annotations": [
        {"id": "one", "startLine": 4, "endLine": 5, "color": "green",
         "text": "the locals"}]}, f)

root = tk.Tk()
root.geometry("1200x800+3000+3000")
app = ca.CodeAnnotatorApp(root, proj)
root.update()
app.open_file(src)
root.update()

# --- enter edit mode ---
app.edit.toggle()
root.update()
assert app.edit.active
assert app.edit.frame.winfo_ismapped() and not app.viewer.code_frame.winfo_ismapped()
assert app.edit.editor.get("1.0", "end-1c") == body
assert app.edit_btn.cget("text").endswith("Done")
assert not app.edit.dirty
# syntax tags applied ('def', 'import' are keywords)
assert app.edit.editor.tag_ranges("tok_kw"), "no keyword highlighting in editor"
# annotation band tag covers lines 4-5
rng = app.edit.editor.tag_ranges("eband_one")
assert str(rng[0]) == "4.0" and str(rng[1]) == "6.0", rng
# gutter drew line numbers
root.update()
assert app.edit.gutter.find_all(), "no line numbers in edit gutter"
print("enter edit mode OK")

# --- type a change: insert two lines above the annotation ---
app.edit.editor.insert("2.0", "VERSION = 3\nNAME = 'x'\n")
root.update()
assert app.edit.dirty, "modification not detected"
assert "●" in app.status_file.cget("text")
app.edit._apply_highlight()       # run the debounced highlight now
# the new lines got tokenized (NAME string literal)
found_str = any("2.0" <= str(i) for i in app.edit.editor.tag_ranges("tok_str"))
assert found_str

# --- undo / redo ---
app.edit.editor.edit_undo()
root.update()
assert app.edit.editor.get("1.0", "end-1c") == body, "undo did not restore"
app.edit.editor.edit_redo()
root.update()
assert "VERSION = 3" in app.edit.editor.get("1.0", "end-1c"), "redo did not reapply"
print("undo/redo OK")

# --- save: file written, annotation re-anchored via its band tag ---
app.edit.save()
root.update()
on_disk = open(src, encoding="utf-8").read()
assert on_disk.startswith("import os\nVERSION = 3\nNAME = 'x'\n"), on_disk
assert not app.edit.dirty
ann = json.load(open(os.path.join(anndir, "sample.py.ann.json"),
                     encoding="utf-8"))["annotations"][0]
assert (ann["startLine"], ann["endLine"]) == (6, 7), ann
print("save + annotation re-anchor OK")

# --- exit edit mode: view refreshes with new content ---
app.edit.toggle()
root.update()
assert not app.edit.active
assert app.viewer.code_frame.winfo_ismapped() and not app.edit.frame.winfo_ismapped()
assert app.lines[1] == "VERSION = 3"
assert app.annotations[0]["startLine"] == 6
print("exit edit mode OK")

# --- discard-changes prompt on exit ---
app.edit.toggle()
root.update()
app.edit.editor.insert("1.0", "junk = True\n")
root.update()
assert app.edit.dirty
mb.askyesnocancel = lambda *a, **k: None       # Cancel
assert app.edit.exit() is False and app.edit.active
mb.askyesnocancel = lambda *a, **k: False      # Discard
assert app.edit.exit() is True and not app.edit.active
assert "junk" not in open(src, encoding="utf-8").read()
print("unsaved-changes prompt (cancel + discard) OK")

# --- CRLF round-trip: saving unchanged content preserves the bytes ---
crlf = os.path.join(proj, "crlf.py")
with open(crlf, "wb") as f:
    f.write(b"a = 1\r\nb = 2\r\n")
app.open_file(crlf)
root.update()
app.edit.toggle()
root.update()
app.edit.editor.insert("end", "c = 3\n")
app.edit.save()
assert open(crlf, "rb").read() == b"a = 1\r\nb = 2\r\nc = 3\r\n"
app.edit.toggle()
root.update()
print("CRLF preservation OK")

# --- non-UTF8 file: editing refused ---
bad = os.path.join(proj, "bad.py")
with open(bad, "wb") as f:
    f.write(b"x = b'\xff\xfe'\nx = '\xff'\n")
app.open_file(bad)
root.update()
app.edit.toggle()
assert not app.edit.active
assert "not valid UTF-8" in app.status_hint.cget("text")
print("non-UTF8 refusal OK")

# --- shortcuts guarded while editing ---
app.open_file(src)
root.update()
app.edit.toggle()
assert app.edit.active
assert app.finder.open_find() == "break" and not app.finder.find_bar.winfo_ismapped()
assert app._goto_annotation(1) is None          # no-op in edit mode
app.edit.toggle()
root.update()

root.destroy()
shutil.rmtree(tmp)
print("ALL EDIT-MODE CHECKS PASSED")
