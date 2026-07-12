# -*- coding: utf-8 -*-
"""Outline panel, color filter, delete undo, settings, portable wheel,
in-editor find, live sidecar reload, and the save-conflict guard."""
import json
import os
import shutil
import sys
import tempfile
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import tkinter as tk
import tkinter.messagebox as mb
import code_annotator as ca

tmp = tempfile.mkdtemp()
proj = os.path.join(tmp, "proj"); os.makedirs(proj)
anndir = os.path.join(proj, ".annotations"); os.makedirs(anndir)
src = os.path.join(proj, "sample.py")
sidecar = os.path.join(anndir, "sample.py.ann.json")
with open(src, "w", encoding="utf-8") as f:
    f.write("import os\n\ndef main():\n    a = 1\n    b = 2\n"
            "    return a + b\n"
            + "".join(f"filler_{i} = {i}\n" for i in range(120)))
with open(sidecar, "w", encoding="utf-8") as f:
    json.dump({"version": 1, "file": "sample.py", "annotations": [
        {"id": "one", "startLine": 2, "endLine": 2, "color": "green",
         "text": "gap line", "anchor": ""},
        {"id": "two", "startLine": 4, "endLine": 5, "color": "orange",
         "text": "locals", "anchor": "a = 1"}]}, f)

settings_path = os.path.join(tmp, "settings.json")
root = tk.Tk(); root.geometry("1200x800+3000+3000")
app = ca.CodeAnnotatorApp(root, proj, settings_path=settings_path)
root.update()
app.open_file(src); root.update()

# --- portable wheel handling ---
up_x11 = types.SimpleNamespace(num=4, delta=0, state=0)
down_x11 = types.SimpleNamespace(num=5, delta=0, state=0)
up_win = types.SimpleNamespace(num=0, delta=120, state=0)
assert ca.wheel_steps(up_x11) == 1 and ca.wheel_steps(down_x11) == -1
assert ca.wheel_steps(up_win) == 1
app.viewer._scroll_home_end(0)
app.viewer._on_wheel(down_x11)                 # X11-style scroll down
assert app.viewer.canvas.canvasy(0) > 0
app.viewer._on_wheel(up_x11)
print("portable wheel OK")

# --- outline panel lists annotations; clicking jumps ---
outline = app.tree_pane.outline
assert outline.size() == 2, outline.size()
assert "locals" in outline.get(1)
app.show_annotation(1)
assert app._ann_idx == 1
assert "Annotation 2 of 2" in app.status_hint.cget("text")
print("outline panel OK")

# --- color filter hides bands without touching the data ---
assert app.viewer.canvas.find_withtag("ann_two")
app._toggle_color("orange")
assert "orange" not in app.visible_colors
assert not app.viewer.canvas.find_withtag("ann_two"), "filtered band still drawn"
assert app.viewer.canvas.find_withtag("ann_one"), "unfiltered band vanished"
assert len(app.annotations) == 2        # data untouched
app._toggle_color("orange")
assert app.viewer.canvas.find_withtag("ann_two")
print("color filter OK")

# --- delete undo ---
def drive(action):
    for w in root.winfo_children():
        if isinstance(w, ca.AnnotationDialog):
            getattr(w, action)()
root.after(200, drive, "_delete")
app._edit_annotation(app.annotations[1], is_new=False, x=200, y=200)
root.update()
assert len(app.annotations) == 1
assert "Ctrl+Z restores" in app.status_hint.cget("text")
assert app._undo_delete() == "break"
assert len(app.annotations) == 2
assert "locals" in json.load(open(sidecar, encoding="utf-8")
                             )["annotations"][1]["text"]
print("delete undo OK")

# --- in-editor find ---
app.edit.toggle(); root.update()
assert app._open_find() == "break"      # Ctrl+F dispatches to the editor
root.update()
assert app.edit._find_bar.winfo_ismapped()
app.edit._find_entry.insert(0, "a")
app.edit._find_refresh()
hits = len(app.edit._find_hits)
assert hits >= 3, hits
assert app.edit._find_count.cget("text") == "1 of %d" % hits
app.edit.find_next()
assert app.edit._find_count.cget("text") == "2 of %d" % hits
app.edit.find_prev()
assert app.edit._find_count.cget("text") == "1 of %d" % hits
app.edit.close_find()
assert not app.edit.editor.tag_ranges("efind")
print("in-editor find OK")

# --- save-conflict guard: file changed on disk while editing ---
with open(src, "a", encoding="utf-8") as f:
    f.write("# external change\n")
st = os.stat(src)
os.utime(src, (st.st_atime, st.st_mtime + 10))   # ensure mtime moved
asked = []
mb.askyesno = lambda *a, **k: (asked.append(1), False)[1]
app.edit.editor.insert("end", "mine = 1\n")
assert app.edit.save() == "break"
assert asked, "conflict prompt not shown"
assert "external change" in open(src, encoding="utf-8").read()
mb.askyesno = lambda *a, **k: True                # user chooses overwrite
app.edit.save()
assert "mine = 1" in open(src, encoding="utf-8").read()
app.edit.dirty = False
app.edit.toggle(); root.update()
print("save-conflict guard OK")

# --- live sidecar reload while the file is open in the viewer ---
data = json.load(open(sidecar, encoding="utf-8"))
data["annotations"].append({"id": "ext", "startLine": 1, "endLine": 1,
                            "color": "pink", "text": "from outside"})
with open(sidecar, "w", encoding="utf-8") as f:
    json.dump(data, f)
st = os.stat(sidecar)
os.utime(sidecar, (st.st_atime, st.st_mtime + 10))
root.after(int(ca.SIDECAR_POLL_MS * 2.2), root.quit)
root.mainloop()
assert any(a["id"] == "ext" for a in app.annotations), "external ann not loaded"
assert app.tree_pane.outline.size() == 3
print("live sidecar reload OK")

# --- settings persist across sessions ---
app.opacity.set(70)
app._zoom(2)
app._toggle_color("pink")
app._on_close()                          # saves settings + destroys root

root2 = tk.Tk(); root2.geometry("+3000+3000")
app2 = ca.CodeAnnotatorApp(root2, None, settings_path=settings_path)
root2.update()
assert app2.workspace.folder == os.path.abspath(proj)   # last folder restored
assert app2.opacity.get() == 70
assert app2.code_font.cget("size") == 13
assert "pink" not in app2.visible_colors
root2.destroy()
print("settings persistence OK")

shutil.rmtree(tmp)
print("ALL FEATURE CHECKS PASSED")
