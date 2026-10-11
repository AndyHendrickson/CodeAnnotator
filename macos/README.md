# Code Annotator — macOS port

A native Cocoa port of the annotator, sharing the platform-independent core
(`../cpp/core.*`, `../cpp/highlight.*`, `../cpp/json.hpp`) with the Windows
port. Objective-C++ + Cocoa only; no external dependencies.

> **Status:** written to mirror the verified Windows port, but authored on a
> Windows machine — it has not yet been compiled or run on real macOS
> hardware. The shared core (sidecar I/O, anchors, `--check`) is fully
> tested; expect at most minor fix-ups in `main.mm` on first build.

## Build

Requires the Xcode command line tools (`xcode-select --install`):

```sh
cd macos
make            # builds build/code_annotator and build/CodeAnnotator.app
```

## Run

```sh
build/code_annotator                    # or open build/CodeAnnotator.app
build/code_annotator ~/path/to/project
build/code_annotator ~/path/to/project -a ~/notes/project-anns
build/code_annotator ~/path/to/project --check        # validate (no GUI)
build/code_annotator ~/path/to/project --check --fix  # repair drift
```

Settings live in `~/.code_annotator_cpp.json` (same schema as the Windows
port). Annotation sidecars are byte-identical across all three
implementations (Python, Windows C++, macOS C++), so they can be mixed
freely on one project.

## Shortcuts (macOS conventions)

| Action | Keys |
| --- | --- |
| Find | `Cmd+F` (`Cmd+G` / `Shift+Cmd+G` next/previous) |
| Go to line | `Cmd+L` |
| Copy selected lines | `Cmd+C` |
| Restore deleted annotation | `Cmd+Z` |
| Zoom | `Cmd+=` / `Cmd+-` / `Cmd+0`, `Cmd+scroll`, pinch |
| Next / previous annotation | `Option+Down` / `Option+Up` |
| Dismiss find/selection | `Esc` |
| Edit source / Done | `Cmd+E` (or *✎ Edit* in the toolbar) |
| Save source (while editing) | `Cmd+S` |
| Undo / redo (while editing) | `Cmd+Z` / `Shift+Cmd+Z` |

Annotating works like the other ports: click or drag on the line numbers,
then write a note and pick a color; click an existing highlight (or its
note) to edit or delete it.

*✎ Edit* swaps in a native text editor (`NSTextView`) over the same file,
with syntax colors, undo/redo, and the usual clipboard keys; annotation
bands ride along with the text and their line positions are saved with the
file, exactly like the Python and Windows editors. Editing requires valid
UTF-8 (BOM and newline style are preserved); *✓ Done*, switching files, and
quitting prompt about unsaved changes. Like the Windows port, "insert note
as comment" is not included.
