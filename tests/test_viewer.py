# -*- coding: utf-8 -*-
# Copyright (c) 2026 Andy Hendrickson
# SPDX-License-Identifier: MIT
"""Drive the new editor features off-screen and assert on observable state."""
import sys, types
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import tkinter as tk
import code_annotator as ca

root = tk.Tk()
root.geometry("+3000+3000")
app = ca.CodeAnnotatorApp(root, ROOT)
root.update_idletasks()

app.open_file(os.path.join(ROOT, "code_annotator.py"))
root.update()

# --- find ---
app.finder.open_find()
app.finder.find_entry.insert(0, "annotation")
app.finder.refresh()
assert app.finder.matches, "no find matches"
assert app.viewer.canvas.find_withtag("find"), "no find highlight rects"
first_count = app.finder.find_count.cget("text")
assert first_count.startswith("1 of "), first_count
top_before = app.viewer.canvas.canvasy(0)
app.finder.next()
assert app.finder.find_count.cget("text").startswith("2 of ")
app.finder.prev()
assert app.finder.find_count.cget("text").startswith("1 of ")
print("find:", first_count.replace("1 of ", "") , "matches, nav OK")

# scrolled the current match into view when needed
ln = app.finder.matches[app.finder.match_idx][0]
y = app.pad + (ln - 1) * app.line_h
top = app.viewer.canvas.canvasy(0)
assert top <= y <= top + app.viewer.canvas.winfo_height(), "match not in view"

# --- goto line ---
app.finder.open_goto()
app.finder.goto_entry.insert(0, "500")
app.finder.goto_line()
top = app.viewer.canvas.canvasy(0)
y500 = app.pad + 499 * app.line_h
assert top <= y500 <= top + app.viewer.canvas.winfo_height(), "line 500 not visible"
assert app.viewer._sel == (500, 500), app.viewer._sel
print("goto line 500 OK, selection:", app.viewer._sel)

# --- selection + copy ---
class Ev:
    x = 10, ; y = 10; state = 0
ev = types.SimpleNamespace(x=10, y=int(y500 - top) + app.line_h // 2, state=0)
app.viewer.set_selection((500, 502))
app.viewer.copy_selection()
clip = root.clipboard_get()
assert clip == "\n".join(app.lines[499:502]), "clipboard mismatch"
assert "Copied 3 lines" in app.status_hint.cget("text")
print("copy 3 lines OK")

# click-selection via handlers
ev = types.SimpleNamespace(x=50, y=40, state=0)
app.viewer._code_press(ev); app.viewer._code_release(ev)
line = app.viewer._canvas_line(ev)
assert app.viewer._sel == (line, line), (app.viewer._sel, line)
ev2 = types.SimpleNamespace(x=50, y=40 + 3 * app.line_h, state=0)
app.viewer._code_press(ev); app.viewer._code_drag(ev2); app.viewer._code_release(ev2)
assert app.viewer._sel == (line, line + 3), app.viewer._sel
assert app.viewer._dragged
print("click/drag selection OK:", app.viewer._sel)

# --- zoom ---
size0, lh0 = app.code_font.cget("size"), app.line_h
app._zoom(2)
assert app.code_font.cget("size") == size0 + 2 and app.line_h > lh0
assert app.viewer.canvas.find_withtag("code"), "code gone after zoom re-render"
assert app.viewer.canvas.find_withtag("find"), "find highlights gone after zoom"
assert app.viewer.canvas.find_withtag("sel"), "selection gone after zoom"
app._zoom(0)
assert app.code_font.cget("size") == 11
print("zoom in/reset OK")

# --- keyboard scrolling ---
app.viewer._scroll_home_end(0)
assert app.viewer.canvas.canvasy(0) < app.line_h
app.viewer._scroll_pages(1)
assert app.viewer.canvas.canvasy(0) > 0
app.viewer._scroll_lines(5)
app.viewer._scroll_home_end(1)
assert app.viewer.canvas.canvasy(0) > 1000
print("keyboard scrolling OK")

# --- escape closes and cleans up ---
app._close_bars()
assert not app.finder.find_bar.winfo_ismapped() and not app.finder.goto_bar.winfo_ismapped()
assert not app.viewer.canvas.find_withtag("find") and not app.viewer.canvas.find_withtag("sel")
assert app.viewer._sel is None
print("escape/cleanup OK")

# annotations still render alongside everything
app.viewer.draw_annotations()
assert app.viewer.canvas.find_withtag("ann")
root.destroy()
print("ALL GUI FEATURE CHECKS PASSED")
