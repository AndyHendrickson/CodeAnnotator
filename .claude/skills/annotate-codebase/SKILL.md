---
name: annotate-codebase
description: Create or update Code Annotator annotation files for source files in a project. Use when asked to annotate a codebase, explain code with overlay notes, or generate .ann.json files.
---

# Annotate a codebase for Code Annotator

Annotations are JSON files in a **parallel tree** that mirrors the source
layout. They are displayed by `code_annotator.py` as translucent highlight
bands over the code. Never modify the source files themselves.

## Where annotation files go

1. Determine the base: the **git repo root** containing the file, or the
   project folder if not in a repo.
2. The annotation root is `<base>/.annotations` (unless the user names another).
3. For source file `<base>/src/foo.py`, write `<ann_root>/src/foo.py.ann.json`.
   Create intermediate directories as needed.

## File format

```json
{
  "version": 1,
  "file": "src/foo.py",
  "annotations": [
    {
      "id": "a1b2c3d4",
      "startLine": 10,
      "endLine": 14,
      "color": "yellow",
      "text": "Retry loop — 429s trigger linear backoff instead of failing"
    }
  ]
}
```

- `file`: source path relative to the base, forward slashes.
- `id`: any short unique string (8 hex chars is conventional).
- `startLine`/`endLine`: **1-based, inclusive**, must satisfy
  `1 <= startLine <= endLine <= line count of the file`.
- `color`: one of `yellow`, `green`, `blue`, `pink`, `purple`, `orange`.
- `text`: single line shown floating over the code (newlines are flattened).
- `anchor` (optional but recommended): the text of the first annotated line
  with all whitespace runs collapsed to single spaces. The app uses it to
  re-locate the annotation automatically if the file changes later, and
  `code_annotator.py --check` uses it to detect drift.
- Sort annotations by `startLine`. Overlaps are allowed but hard to read —
  prefer non-overlapping ranges.

## Color convention

- `blue` — configuration, constants, data definitions
- `green` — core algorithm / business logic
- `yellow` — I/O, persistence, network
- `orange` — gotchas, invariants, sign conventions, validation
- `pink` — helpers and utilities
- `purple` — entry points and wiring

## Getting line numbers right

Line numbers must match the file exactly. Read the file with line numbers
(`cat -n` or the Read tool) and take ranges from that output — never estimate
from memory. If generating many files, prefer a script that locates ranges by
searching for anchor text, then emits the JSON. Re-verify after any edit to
the source file, since edits shift line numbers.

## Writing good notes

- One sentence; state what the block does **and** why it matters or what's
  non-obvious ("Core sort: priority first, then FIFO within a priority").
- Annotate meaningful units (a function, a config block, a tricky loop), not
  every line. 3–8 annotations per file is typical; skip trivial files.
- Flag surprises with `orange` — sign conventions, silent fallbacks,
  ordering requirements.

## Checklist before finishing

- [ ] JSON is valid; every annotation has all five fields.
- [ ] Paths mirror the source tree under the annotation root.
- [ ] Line ranges verified against the current file contents.
- [ ] View them: `python code_annotator.py <folder>` (files with annotations
      show a ● badge in the tree).
