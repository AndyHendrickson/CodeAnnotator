# Code Annotator — C++ port

A native Win32 port of `code_annotator.py`: browse a project folder, view
code files with syntax colorization, and overlay translucent annotations
kept in a parallel tree. Like the Python original it has **no external
dependencies** — just the Win32 API and the C++17 standard library.

## Build

Requires the MSVC toolchain (the script uses Visual Studio 18 Community):

```bat
cpp\build.bat
```

This produces `cpp\build\code_annotator.exe`. To use a different Visual
Studio, change the `vcvars64.bat` path at the top of `build.bat`.

## Run

```sh
cpp\build\code_annotator.exe                      # then "Open Folder…"
cpp\build\code_annotator.exe C:\path\to\project
cpp\build\code_annotator.exe C:\path\to\project -a D:\notes\project-anns
cpp\build\code_annotator.exe C:\path\to\project --check        # validate (no GUI)
cpp\build\code_annotator.exe C:\path\to\project --check --fix  # repair drift
```

Window geometry, zoom, opacity, color filter, and the last opened folder are
remembered in `~\.code_annotator_cpp.json` (a separate file from the Python
app's settings, but the same idea).

## Compatibility with the Python app

The two implementations share the same on-disk format and can be used
interchangeably on the same project:

- Annotation sidecars (`<ann_root>/<rel-path>.ann.json`) are read and
  written **byte-identically** to Python's `json.dump(indent=2)` output —
  including `ensure_ascii` escaping and Windows CRLF newlines — so switching
  tools produces no spurious diffs.
- Anchors, orphaning, reconciliation (self-healing after outside edits), the
  `--check`/`--fix` report format, and exit codes match the Python behavior.
- The annotation root defaults to `.annotations/` under the git repo root
  containing the opened folder (or the folder itself outside a repo), same
  as Python; `-a/--annotations` overrides it.

## Feature parity

Ported: the folder tree with lazy loading and `●` badges, the annotation
outline pane, syntax colorization (same languages and colors), translucent
bands with word-wrapped notes, gutter click/drag to annotate, the edit
dialog with color swatches, delete with Ctrl+Z undo, find (`Ctrl+F`, `F3`),
go-to-line (`Ctrl+G`), annotation navigation (`Alt+Up`/`Alt+Down`), line
selection and `Ctrl+C` copy, zoom (`Ctrl+wheel`, `Ctrl+=`/`-`/`0`), the
opacity slider and per-color visibility filter, live sidecar reload while a
file is open, and binary/oversize-file guards.

**Edit mode** is ported too: *✎ Edit* in the toolbar swaps in a source
editor over the same file — syntax colors (re-scanned incrementally as you
type), undo/redo (`Ctrl+Z`, `Ctrl+Y`/`Ctrl+Shift+Z`), cut/copy/paste,
`Ctrl+A`, word navigation, and `Ctrl+F`/`Ctrl+G` within the editor. `Ctrl+S`
or *Save* writes the file atomically; *✓ Done* returns to the annotate view,
prompting about unsaved changes (as do switching files, opening a folder and
closing the window). Annotation bands ride along with the text and their
line positions are saved with the file; an annotated region deleted
entirely collapses to the deletion point and is marked orphaned. Editing
requires valid UTF-8 (BOM and newline style are preserved) and asks before
overwriting a file that changed on disk.

**Not ported** (use the Python app for this): "insert note as comment".

## Source layout

| File | Contents |
| --- | --- |
| `json.hpp` | minimal JSON parser + Python-compatible serializer |
| `core.hpp/.cpp` | annotations, anchors, workspace, atomic saves, `--check` |
| `highlight.hpp/.cpp` | tokenizer and language tables |
| `gui.cpp` | Win32 GUI and `wWinMain` |
