# -*- coding: utf-8 -*-
"""Verify Alt+Up/Alt+Down annotation navigation."""
import sys
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import tkinter as tk
import code_annotator as ca

root = tk.Tk()
root.geometry("1400x900+3000+3000")
app = ca.CodeAnnotatorApp(root, ROOT)
root.update()
app.open_file(os.path.join(ROOT, "code_annotator.py"))
root.update()

anns = app.annotations
n = len(anns)
assert n >= 3, "need several annotations to test with"

# from the top of the file, Alt+Down goes to the first annotation below view
app._goto_annotation(1)
root.update()
assert app._ann_idx is not None
first_idx = app._ann_idx
ln = anns[first_idx]["startLine"]
assert app.viewer.line_visible(ln), "jump target not scrolled into view"
hint = app.status_hint.cget("text")
assert hint.startswith("Annotation %d of %d" % (first_idx + 1, n)), hint
print("first jump ->", hint)

# repeated Alt+Down walks forward one annotation at a time
app._goto_annotation(1)
root.update()
assert app._ann_idx == (first_idx + 1) % n
assert app.viewer.line_visible(anns[app._ann_idx]["startLine"])

# Alt+Up walks back
app._goto_annotation(-1)
root.update()
assert app._ann_idx == first_idx

# wrap-around: walk forward through all annotations back to start
seen = []
for _ in range(n):
    app._goto_annotation(1)
    root.update()
    seen.append(app._ann_idx)
assert app._ann_idx == first_idx, (seen, first_idx)
assert sorted(seen) == list(range(n)), "did not visit every annotation once"
print("cycle through all %d annotations with wrap-around OK" % n)

# after scrolling away manually, the next jump re-anchors to the viewport
app.viewer._scroll_home_end(1)   # jump to end of file
root.update()
app._goto_annotation(-1)  # previous = last annotation above the bottom view
root.update()
assert app._ann_idx == n - 1, app._ann_idx
assert app.viewer.line_visible(anns[-1]["startLine"])
print("re-anchor after manual scroll OK")

# flash rectangle appears and is cleaned up by its timer
root.after(900, root.quit)
root.mainloop()          # drain flash timers from the jumps above
items_before = len(app.viewer.canvas.find_all())
app._goto_annotation(1)
assert len(app.viewer.canvas.find_all()) == items_before + 1, "no flash drawn"
root.after(900, root.quit)
root.mainloop()          # let the 700ms cleanup timer fire
assert len(app.viewer.canvas.find_all()) == items_before, "flash not cleaned up"
print("flash + cleanup OK")

# real key events through the actual bindings
root.focus_force()
app.viewer.canvas.focus_set()
root.update()
before = app._ann_idx
app.viewer.canvas.event_generate("<Alt-Down>")
root.update()
assert app._ann_idx != before or len(app.annotations) == 1, "binding did not fire"
print("real <Alt-Down> key event OK")

# no file open: must be a no-op, not a crash
app._show_message("x")
app._goto_annotation(1)
app._goto_annotation(-1)
print("no-file no-op OK")

root.destroy()
print("ALL ANNOTATION-NAV CHECKS PASSED")
