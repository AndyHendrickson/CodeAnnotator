# -*- coding: utf-8 -*-
"""Verify note wrapping: CR handling, code avoidance, re-wrap on resize/scroll."""
import json, os, sys, tempfile
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import tkinter as tk
import code_annotator as ca

# --- CR normalization straight through the loader ---
tmp = tempfile.mkdtemp()
p = os.path.join(tmp, "x.ann.json")
with open(p, "w", encoding="utf-8") as f:
    json.dump({"version": 1, "file": "x", "annotations": [
        {"id": "aa", "startLine": 1, "endLine": 2,
         "text": "first\r\nsecond\rthird", "color": "blue"}]}, f)
loaded = ca.load_annotations(p)
assert loaded[0]["text"] == "first\nsecond\nthird", repr(loaded[0]["text"])
print("CR normalization OK")

root = tk.Tk()
root.geometry("1400x900+3000+3000")
app = ca.CodeAnnotatorApp(root, ROOT)
root.update()
app.open_file(os.path.join(ROOT, "code_annotator.py"))
root.update()

# add a long, multi-paragraph note on lines with short code so wrapping is visible
long_text = ("This is a deliberately long annotation note that must wrap "
             "across several display lines because it keeps going and going "
             "well past any plausible single-line width in the viewport, "
             "repeating itself for good measure and then some more.\n"
             "It also contains an explicit line break which should be "
             "preserved rather than flattened into spaces.")
app.annotations.append({"id": "testnote", "startLine": 26, "endLine": 27,
                        "color": "green", "text": long_text})
app.viewer.draw_annotations()
root.update()

def note_item():
    for it in app.viewer.canvas.find_withtag("note"):
        if "ann_testnote" in app.viewer.canvas.gettags(it):
            return it
    raise AssertionError("test note not drawn")

it = note_item()
assert "\n" in app.viewer.canvas.itemcget(it, "text"), "line break flattened"
w1 = int(float(app.viewer.canvas.itemcget(it, "width")))
assert w1 > 0, "no wrap width set"
x0, y0, x1, y1 = app.viewer.canvas.bbox(it)
note_lh = app.note_font.metrics("linespace")
n_note_lines = round((y1 - y0) / float(note_lh))
assert n_note_lines >= 3, "expected >=3 note lines, got %d" % n_note_lines

# the note must clear the code on every line it covers (when space allowed)
lo = 26
covered_hi = min(len(app.lines), int((y1 - app.pad) // app.line_h) + 1)
code_right = app.pad + max(app.viewer._line_cols[lo - 1:covered_hi]) * app.char_w
if w1 > 80:  # not clamped: overlap must be avoided
    assert x0 >= code_right, (x0, code_right)
print("wrap + code avoidance OK: width=%d, note spans y %d..%d, code right=%d"
      % (w1, y0, y1, code_right))

# right edge hugs the viewport
vx = app.viewer.canvas.canvasx(app.viewer.canvas.winfo_width()) - 12
assert abs(x1 - vx) < 30, (x1, vx)

# --- resize re-wraps ---
root.geometry("1000x900+3000+3000")
root.update()
it = note_item()
w2 = int(float(app.viewer.canvas.itemcget(it, "width")))
assert w2 < w1, ("resize did not re-wrap", w1, w2)
print("resize re-wrap OK: %d -> %d" % (w1, w2))

# --- horizontal scroll re-wraps and follows the viewport ---
app.viewer.canvas.xview_moveto(0.3)
root.update()
it = note_item()
x1b = app.viewer.canvas.bbox(it)[2]
vx = app.viewer.canvas.canvasx(app.viewer.canvas.winfo_width()) - 12
assert abs(x1b - vx) < 30, (x1b, vx)
print("h-scroll re-wrap OK")

# hopeless gap: note over very long code lines clamps but still renders
app.viewer.canvas.xview_moveto(0)
longest = max(range(len(app.viewer._line_cols)), key=lambda i: app.viewer._line_cols[i]) + 1
app.annotations.append({"id": "clampnote", "startLine": longest,
                        "endLine": longest, "color": "pink",
                        "text": "note over the longest line " * 3})
app.viewer.draw_annotations()
root.update()
found = [it for it in app.viewer.canvas.find_withtag("note")
         if "ann_clampnote" in app.viewer.canvas.gettags(it)]
assert found and int(float(app.viewer.canvas.itemcget(found[0], "width"))) >= 80
print("clamped-width fallback OK")

root.destroy()
print("ALL NOTE-WRAP CHECKS PASSED")
