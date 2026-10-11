#!/usr/bin/env python3
# Copyright (c) 2026 Andy Hendrickson
# SPDX-License-Identifier: MIT
"""Code Annotator — browse a project folder, view code files with syntax
colorization, and overlay translucent annotations kept in a parallel tree.

Annotations never live next to the source. They mirror the source layout
under an annotation root: for ``<base>/src/foo.py`` the annotations are in
``<ann_root>/src/foo.py.ann.json``, where ``<base>`` is the root of the git
repository containing the opened folder (or the opened folder itself when
not in a repo) and ``<ann_root>`` defaults to ``<base>/.annotations``.

    {
      "version": 1,
      "file": "src/foo.py",
      "annotations": [
        {"id": "a1b2", "startLine": 10, "endLine": 14,
         "color": "yellow", "text": "This block handles auth"}
      ]
    }

Usage:  python code_annotator.py [folder] [-a/--annotations ANN_ROOT]
        python code_annotator.py [folder] --check [--fix]

Requires only the Python standard library (tkinter). Runs on Python 3.8+
on Windows, Linux and macOS.
"""

import argparse
import json
import os
import sys
import uuid
import tkinter as tk
from tkinter import ttk, filedialog, font as tkfont

# ---------------------------------------------------------------- constants

SIDECAR_SUFFIX = ".ann.json"
ANN_DIR_NAME = ".annotations"
MAX_FILE_BYTES = 2 * 1024 * 1024
MAX_LINES = 8000
TAB_WIDTH = 4
SIDECAR_POLL_MS = 1500          # how often to watch the open file's sidecar

# preferred font families, best-first; the first installed one wins
CODE_FONTS = ("Consolas", "Menlo", "DejaVu Sans Mono", "Liberation Mono",
              "Courier New")
UI_FONTS = ("Segoe UI", "Helvetica Neue", "DejaVu Sans", "Helvetica")
UI_FONT = UI_FONTS[0]           # re-picked at startup for the platform

SKIP_DIRS = {".git", ".hg", ".svn", "node_modules", "__pycache__", ".vs",
             ".idea", "bin", "obj", "dist", "build", ".next", "venv",
             ".venv", "target", ".mypy_cache", ".pytest_cache"}

BG = "#1e1e1e"
BG_PANEL = "#252526"
BG_ACTIVE = "#37373d"
FG = "#d4d4d4"
FG_DIM = "#858585"
GUTTER_FG = "#6e7681"
ACCENT = "#0e639c"

TOKEN_COLORS = {
    "com": "#6a9955",
    "str": "#ce9178",
    "num": "#b5cea8",
    "kw":  "#c586c0",
    "lit": "#569cd6",
    "typ": "#4ec9b0",
    "fn":  "#dcdcaa",
    "tag": "#569cd6",
    "attr": "#9cdcfe",
    None:  FG,
}

ANN_COLORS = {          # annotation highlight palette (full-strength RGB)
    "yellow": (230, 190, 50),
    "green":  (80, 200, 120),
    "blue":   (70, 150, 255),
    "pink":   (240, 100, 170),
    "purple": (170, 110, 255),
    "orange": (250, 140, 60),
}

# ------------------------------------------------------ syntax highlighting

KEYWORDS = {
    "py": """and as assert async await break class continue def del elif else
        except finally for from global if import in is lambda nonlocal not or
        pass raise return try while with yield match case""",
    "js": """abstract as async await break case catch class const continue
        debugger default delete do else enum export extends finally for from
        function get if implements import in instanceof interface let new of
        private protected public return set static super switch this throw
        try type typeof var void while with yield""",
    "cs": """abstract as base break case catch checked class const continue
        default delegate do else enum event explicit extern finally fixed for
        foreach goto if implicit in interface internal is lock namespace new
        operator out override params private protected public readonly ref
        return sealed static struct switch this throw try typeof unsafe using
        var virtual volatile while get set async await record init required
        partial when where yield""",
    "c": """auto break case const continue default do else enum extern for
        goto if inline register return signed sizeof static struct switch
        typedef union unsigned volatile while class delete friend namespace
        new operator private protected public template this throw try catch
        typename using virtual override final constexpr nullptr""",
    "java": """abstract assert break case catch class const continue default
        do else enum extends final finally for goto if implements import
        instanceof interface native new package private protected public
        return static strictfp super switch synchronized this throw throws
        transient try volatile while var record sealed permits yield""",
    "go": """break case chan const continue default defer else fallthrough
        for func go goto if import interface map package range return select
        struct switch type var""",
    "rust": """as async await break const continue crate dyn else enum extern
        fn for if impl in let loop match mod move mut pub ref return self
        static struct super trait type unsafe use where while""",
    "sql": """select from where insert update delete into values set join
        left right inner outer on group by order having limit offset union
        all distinct as and or not null is in like between exists case when
        then else end create table drop alter index view primary key foreign
        references default unique check""",
    "sh": """if then else elif fi for while until do done case esac function
        in select return exit break continue local declare export readonly
        unset shift eval exec source alias""",
}

LITERALS = {
    "py": "True False None self cls",
    "js": "true false null undefined NaN Infinity",
    "cs": "true false null this value",
    "c": "true false NULL nullptr this",
    "java": "true false null this",
    "go": "true false nil iota",
    "rust": "true false Self self None Some Ok Err",
    "sql": "true false null",
    "sh": "true false",
}

TYPES = {
    "py": """int float str bool list dict set tuple bytes object Exception
        print len range type super isinstance""",
    "js": """boolean number string object symbol bigint any unknown never
        Array Object String Number Boolean Promise Map Set Date RegExp Error
        console window document Math JSON""",
    "cs": """bool byte char decimal double float int long object sbyte short
        string uint ulong ushort void Task List Dictionary IEnumerable String
        Console Exception""",
    "c": """bool char double float int long short signed unsigned void size_t
        int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t std
        string vector map""",
    "java": """boolean byte char double float int long short void String
        Integer Boolean Object List Map ArrayList HashMap System Exception""",
    "go": """bool byte complex64 complex128 error float32 float64 int int8
        int16 int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr""",
    "rust": """bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64
        u128 usize String Vec Option Result Box HashMap""",
    "sql": """int integer bigint smallint varchar char text date datetime
        timestamp decimal numeric float real boolean blob""",
    "sh": "echo printf read cd ls rm mv cp mkdir grep sed awk cat test",
}


def _word_map(lang_id):
    words = {}
    for w in KEYWORDS.get(lang_id, "").split():
        words[w] = "kw"
    for w in LITERALS.get(lang_id, "").split():
        words[w] = "lit"
    for w in TYPES.get(lang_id, "").split():
        words[w] = "typ"
    return words


def _lang(lang_id, **opts):
    cfg = {"words": _word_map(lang_id), "strings": "'\"",
           "line": None, "block": None, "triple": False,
           "template": False, "casefold": False, "html": False}
    cfg.update(opts)
    return cfg


LANGS = {
    "py":   _lang("py", line="#", triple=True),
    "js":   _lang("js", line="//", block=("/*", "*/"), template=True),
    "cs":   _lang("cs", line="//", block=("/*", "*/")),
    "c":    _lang("c", line="//", block=("/*", "*/")),
    "java": _lang("java", line="//", block=("/*", "*/")),
    "go":   _lang("go", line="//", block=("/*", "*/"), template=True),
    "rust": _lang("rust", line="//", block=("/*", "*/")),
    "sql":  _lang("sql", line="--", block=("/*", "*/"), casefold=True),
    "sh":   _lang("sh", line="#"),
    "css":  _lang("css", block=("/*", "*/")),
    "json": _lang("json"),
    "html": {"html": True},
    "plain": None,
}
LANGS["json"]["words"] = {"true": "lit", "false": "lit", "null": "lit"}

EXT_LANG = {
    "py": "py", "pyw": "py", "rb": "py", "lua": "py", "r": "py",
    "js": "js", "jsx": "js", "ts": "js", "tsx": "js", "mjs": "js",
    "cjs": "js", "vue": "js", "php": "js", "swift": "js", "dart": "js",
    "cs": "cs",
    "c": "c", "h": "c", "cpp": "c", "hpp": "c", "cc": "c", "cxx": "c",
    "ino": "c",
    "java": "java", "kt": "java", "kts": "java", "scala": "java",
    "go": "go",
    "rs": "rust",
    "sql": "sql",
    "sh": "sh", "bash": "sh", "zsh": "sh", "ps1": "sh", "psm1": "sh",
    "bat": "sh", "cmd": "sh", "yml": "sh", "yaml": "sh", "toml": "sh",
    "ini": "sh", "env": "sh", "gitignore": "sh",
    "css": "css", "scss": "css", "less": "css",
    "json": "json", "jsonc": "json",
    "html": "html", "htm": "html", "xml": "html", "svg": "html",
    "xaml": "html", "csproj": "html", "config": "html",
}


def lang_for(name):
    lower = name.lower()
    if lower in ("dockerfile", "makefile"):
        return "sh"
    ext = lower.rsplit(".", 1)[-1] if "." in lower else ""
    return EXT_LANG.get(ext, "plain")


def _ident_char(c):
    return c.isalnum() or c in "_$"


def scan_line(line, cfg, st):
    """Tokenize one line. Yields (start_col, text, token_class).
    ``st`` is a dict carrying multi-line state across calls."""
    out = []
    i, n = 0, len(line)
    while i < n:
        if st["mode"] == "block":
            end = line.find(cfg["block"][1], i)
            if end < 0:
                out.append((i, line[i:], "com"))
                return out
            end += len(cfg["block"][1])
            out.append((i, line[i:end], "com"))
            i, st["mode"] = end, None
            continue
        if st["mode"] == "str":
            end = line.find(st["delim"], i)
            if end < 0:
                out.append((i, line[i:], "str"))
                return out
            end += len(st["delim"])
            out.append((i, line[i:end], "str"))
            i, st["mode"] = end, None
            continue

        ch = line[i]
        if cfg["block"] and line.startswith(cfg["block"][0], i):
            end = line.find(cfg["block"][1], i + len(cfg["block"][0]))
            if end < 0:
                out.append((i, line[i:], "com"))
                st["mode"] = "block"
                return out
            end += len(cfg["block"][1])
            out.append((i, line[i:end], "com"))
            i = end
            continue
        if cfg["line"] and line.startswith(cfg["line"], i):
            out.append((i, line[i:], "com"))
            return out
        if cfg["triple"] and line[i:i + 3] in ("'''", '"""'):
            delim = line[i:i + 3]
            end = line.find(delim, i + 3)
            if end < 0:
                out.append((i, line[i:], "str"))
                st["mode"], st["delim"] = "str", delim
                return out
            out.append((i, line[i:end + 3], "str"))
            i = end + 3
            continue
        if cfg["template"] and ch == "`":
            j = i + 1
            while j < n and line[j] != "`":
                j += 2 if line[j] == "\\" else 1
            if j >= n:
                out.append((i, line[i:], "str"))
                st["mode"], st["delim"] = "str", "`"
                return out
            out.append((i, line[i:j + 1], "str"))
            i = j + 1
            continue
        if ch in cfg["strings"]:
            j = i + 1
            while j < n and line[j] != ch:
                j += 2 if line[j] == "\\" else 1
            j = min(j + 1, n)
            out.append((i, line[i:j], "str"))
            i = j
            continue
        if ch.isdigit() or (ch == "." and i + 1 < n and line[i + 1].isdigit()):
            j = i
            while j < n and (line[j].isalnum() or line[j] in "._"):
                j += 1
            out.append((i, line[i:j], "num"))
            i = j
            continue
        if ch.isalpha() or ch in "_$":
            j = i
            while j < n and _ident_char(line[j]):
                j += 1
            word = line[i:j]
            key = word.lower() if cfg["casefold"] else word
            cls = cfg["words"].get(key)
            if cls is None and j < n and line[j] == "(":
                cls = "fn"
            out.append((i, word, cls))
            i = j
            continue
        out.append((i, ch, None))
        i += 1
    return out


def scan_html_line(line, st):
    """Minimal HTML/XML tokenizer with cross-line comment/tag state."""
    out = []
    i, n = 0, len(line)
    while i < n:
        if st["mode"] == "comment":
            end = line.find("-->", i)
            if end < 0:
                out.append((i, line[i:], "com"))
                return out
            out.append((i, line[i:end + 3], "com"))
            i, st["mode"] = end + 3, None
            continue
        if st["mode"] == "tag":
            ch = line[i]
            if ch == ">":
                out.append((i, ">", None))
                i, st["mode"] = i + 1, None
                continue
            if ch in "'\"":
                j = i + 1
                while j < n and line[j] != ch:
                    j += 1
                j = min(j + 1, n)
                out.append((i, line[i:j], "str"))
                i = j
                continue
            if ch.isalpha() or ch in "_:-":
                j = i
                while j < n and (line[j].isalnum() or line[j] in "_:.-"):
                    j += 1
                out.append((i, line[i:j], "attr"))
                i = j
                continue
            out.append((i, ch, None))
            i += 1
            continue
        if line.startswith("<!--", i):
            end = line.find("-->", i)
            if end < 0:
                out.append((i, line[i:], "com"))
                st["mode"] = "comment"
                return out
            out.append((i, line[i:end + 3], "com"))
            i = end + 3
            continue
        if line[i] == "<":
            j = i + 1
            if j < n and line[j] in "/!?":
                j += 1
            k = j
            while k < n and (line[k].isalnum() or line[k] in "_:.-"):
                k += 1
            out.append((i, line[i:j], None))
            if k > j:
                out.append((j, line[j:k], "tag"))
            i, st["mode"] = k, "tag"
            continue
        j = line.find("<", i)
        if j < 0:
            j = n
        out.append((i, line[i:j], None))
        i = j
    return out


def highlight_lines(lines, lang_id):
    """Return per-line token lists for the whole file."""
    cfg = LANGS.get(lang_id)
    st = {"mode": None, "delim": ""}
    if cfg is None:
        return [[(0, ln, None)] if ln else [] for ln in lines]
    if cfg.get("html"):
        return [scan_html_line(ln, st) for ln in lines]
    return [scan_line(ln, cfg, st) for ln in lines]


# ------------------------------------------------------------------ helpers

def blend(bg_hex, rgb, alpha):
    """Alpha-blend an RGB tuple over a hex background color."""
    br = int(bg_hex[1:3], 16)
    bg = int(bg_hex[3:5], 16)
    bb = int(bg_hex[5:7], 16)
    r = round(br + (rgb[0] - br) * alpha)
    g = round(bg + (rgb[1] - bg) * alpha)
    b = round(bb + (rgb[2] - bb) * alpha)
    return "#%02x%02x%02x" % (r, g, b)


def pick_font(candidates, available):
    """First candidate family that is installed, else the last one."""
    for name in candidates:
        if name in available:
            return name
    return candidates[-1]


def wheel_steps(event):
    """Direction of a mouse-wheel event as +1 (up) / -1 (down), on any
    platform: Windows/macOS deliver <MouseWheel> with .delta, X11 delivers
    separate <Button-4>/<Button-5> events with .num."""
    if getattr(event, "num", 0) == 4:
        return 1
    if getattr(event, "num", 0) == 5:
        return -1
    return 1 if event.delta > 0 else -1


def bind_mousewheel(widget, handler):
    """Bind wheel scrolling portably (see wheel_steps)."""
    widget.bind("<MouseWheel>", handler)
    widget.bind("<Button-4>", handler)
    widget.bind("<Button-5>", handler)


def anchor_text(line):
    """Whitespace-insensitive fingerprint of a source line, used to
    re-locate annotations after the file changes outside the app."""
    return " ".join(line.split())


def find_git_root(path):
    """Walk up from ``path`` looking for a .git entry; None if not in a repo."""
    cur = os.path.abspath(path)
    while True:
        if os.path.exists(os.path.join(cur, ".git")):
            return cur
        parent = os.path.dirname(cur)
        if parent == cur:
            return None
        cur = parent


class AnnotationLoadError(Exception):
    """An annotation sidecar exists but could not be read or parsed."""


def load_annotations(ann_path):
    """Read one annotation file; [] when it doesn't exist.

    Raises AnnotationLoadError when the file exists but cannot be parsed —
    callers must not mistake a corrupt file for an empty one, or a later
    save would overwrite it and destroy its contents."""
    if not os.path.isfile(ann_path):
        return []
    try:
        with open(ann_path, encoding="utf-8") as f:
            data = json.load(f)
        entries = data.get("annotations", [])
    except (OSError, ValueError, AttributeError) as exc:
        raise AnnotationLoadError("%s: %s" % (ann_path, exc))
    result = []
    for a in entries:
        try:
            start, end = int(a["startLine"]), int(a["endLine"])
        except (KeyError, TypeError, ValueError):
            continue
        if start < 1:
            continue
        text = str(a.get("text", ""))
        text = text.replace("\r\n", "\n").replace("\r", "\n")
        result.append({
            "id": str(a.get("id") or uuid.uuid4().hex[:8]),
            "startLine": start,
            "endLine": max(start, end),
            "color": a.get("color") if a.get("color") in ANN_COLORS else "yellow",
            "text": text,
            "orphaned": bool(a.get("orphaned")),
            "anchor": str(a.get("anchor", "")),
        })
    result.sort(key=lambda a: a["startLine"])
    return result


def save_annotations(ann_path, rel_name, annotations, prune_root=None):
    """Write (or remove) one annotation file inside the parallel tree.

    When the last annotation goes away the file is deleted and any directories
    left empty are pruned up to (but not including) ``prune_root``."""
    if not annotations:
        try:
            os.remove(ann_path)
        except OSError:
            pass
        if prune_root:
            root = os.path.abspath(prune_root)
            d = os.path.abspath(os.path.dirname(ann_path))
            while d != root and d.startswith(root + os.sep):
                try:
                    os.rmdir(d)
                except OSError:
                    break
                d = os.path.dirname(d)
        return
    os.makedirs(os.path.dirname(ann_path), exist_ok=True)
    entries = []
    for a in annotations:                # keep files clean: optional fields
        a = dict(a)                      # are only written when set
        if not a.get("orphaned"):
            a.pop("orphaned", None)
        if not a.get("anchor"):
            a.pop("anchor", None)
        entries.append(a)
    data = {"version": 1, "file": rel_name, "annotations": entries}
    # write to a sibling temp file, then swap atomically — a crash mid-write
    # can never leave a truncated annotation file behind
    tmp = ann_path + ".tmp~"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    os.replace(tmp, ann_path)


def reconcile_annotations(annotations, lines):
    """Self-heal annotations after the source changed outside the app.

    Each annotation may carry an ``anchor`` — a whitespace-insensitive copy
    of its first line. When the anchor no longer matches the annotation's
    startLine, the annotation is moved to the nearest line whose text still
    matches; if the anchored text is gone entirely, it is flagged orphaned.
    Annotations without an anchor adopt one from their current line (in
    memory only) so future drift can be tracked. Returns (repaired, lost)."""
    repaired = lost = 0
    for a in annotations:
        anchor = a.get("anchor", "")
        if not anchor:
            if (not a.get("orphaned")
                    and 1 <= a["startLine"] <= len(lines)):
                a["anchor"] = anchor_text(lines[a["startLine"] - 1])
            continue
        idx = a["startLine"] - 1
        if 0 <= idx < len(lines) and anchor_text(lines[idx]) == anchor:
            continue
        candidates = [i + 1 for i, ln in enumerate(lines)
                      if anchor_text(ln) == anchor]
        if candidates:
            new_start = min(candidates,
                            key=lambda n: abs(n - a["startLine"]))
            span = a["endLine"] - a["startLine"]
            a["startLine"] = new_start
            a["endLine"] = min(new_start + span, len(lines))
            a["orphaned"] = False
            repaired += 1
        elif not a.get("orphaned"):
            a["orphaned"] = True
            a["startLine"] = min(a["startLine"], len(lines))
            a["endLine"] = min(a["endLine"], len(lines))
            lost += 1
    annotations.sort(key=lambda a: a["startLine"])
    return repaired, lost


def read_source_lines(path):
    """Source lines the way the app displays them (tabs expanded)."""
    with open(path, encoding="utf-8", errors="replace") as f:
        return [ln.expandtabs(TAB_WIDTH) for ln in f.read().splitlines()] or [""]


def check_annotations(folder, fix=False, ann_root=None, out=print):
    """CLI: validate every annotation file against its source. Reports
    unparseable sidecars, missing sources, out-of-range line numbers and
    drifted anchors; with ``fix`` the repairable ones are rewritten.
    Returns a process exit code (0 = clean or fully fixed)."""
    workspace = Workspace(ann_root)
    workspace.open(folder)
    if not os.path.isdir(workspace.ann_root):
        out(f"no annotation tree at {workspace.ann_root}")
        return 0
    files = problems = unfixable = 0
    for base, _dirs, names in os.walk(workspace.ann_root):
        for name in sorted(names):
            if not name.endswith(SIDECAR_SUFFIX):
                continue
            files += 1
            ann_path = os.path.join(base, name)
            rel = os.path.relpath(ann_path,
                                  workspace.ann_root)[:-len(SIDECAR_SUFFIX)]
            label = rel.replace(os.sep, "/")
            src = os.path.join(workspace.source_base, rel)
            try:
                annotations = load_annotations(ann_path)
            except AnnotationLoadError as exc:
                out(f"ERROR  {label}: unparseable annotation file ({exc})")
                problems += 1
                unfixable += 1
                continue
            if not os.path.isfile(src):
                out(f"ERROR  {label}: source file is missing")
                problems += 1
                unfixable += 1
                continue
            lines = read_source_lines(src)
            issues = []
            for a in annotations:
                if a["endLine"] > len(lines):
                    issues.append(f"{a['id']}: range {a['startLine']}-"
                                  f"{a['endLine']} beyond {len(lines)} lines")
                elif (a.get("anchor") and not a.get("orphaned")
                        and anchor_text(lines[a["startLine"] - 1])
                        != a["anchor"]):
                    issues.append(f"{a['id']}: anchor drifted from line "
                                  f"{a['startLine']}")
            for issue in issues:
                out(f"WARN   {label}: {issue}")
            problems += len(issues)
            if issues and fix:
                repaired, lost = reconcile_annotations(annotations, lines)
                save_annotations(ann_path, rel.replace(os.sep, "/"),
                                 annotations, prune_root=workspace.ann_root)
                out(f"FIXED  {label}: {repaired} re-anchored, "
                    f"{lost} orphaned")
    out(f"checked {files} annotation file(s): "
        f"{problems} problem(s){' (fix applied)' if fix and problems else ''}")
    if problems == 0:
        return 0
    return 1 if (not fix or unfixable) else 0


def load_settings(path):
    """Best-effort read of the settings JSON; {} when absent or invalid."""
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def comment_lines(text, lang_id):
    """Render annotation text as source-comment lines for a language.
    Returns a list of strings, or None when the language has no comment
    syntax we know (json, plain)."""
    cfg = LANGS.get(lang_id)
    parts = text.split("\n")
    if lang_id == "html":
        return ["<!-- " + p + " -->" for p in parts]
    if cfg and cfg.get("line"):
        return [(cfg["line"] + " " + p).rstrip() for p in parts]
    if cfg and cfg.get("block"):
        return [cfg["block"][0] + " " + p + " " + cfg["block"][1]
                for p in parts]
    return None


# ------------------------------------------------------------- edit dialog

class AnnotationDialog(tk.Toplevel):
    """Small popup to create/edit one annotation. Sets self.result to
    'save', 'delete', or None (cancel)."""

    def __init__(self, master, ann, is_new, x, y, can_paste=False):
        super().__init__(master)
        self.ann = ann
        self.result = None
        self.overrideredirect(True)
        self.configure(bg=BG_PANEL, highlightbackground="#3c3c3c",
                       highlightthickness=1)

        rng = ("line %d" % ann["startLine"] if ann["startLine"] == ann["endLine"]
               else "lines %d–%d" % (ann["startLine"], ann["endLine"]))
        title = ("New annotation — " if is_new else "Edit annotation — ") + rng
        tk.Label(self, text=title, bg=BG_PANEL, fg=FG_DIM,
                 font=(UI_FONT, 9, "bold")).pack(anchor="w", padx=10, pady=(8, 4))

        self.text = tk.Text(self, width=42, height=4, bg=BG, fg=FG,
                            insertbackground=FG, relief="flat",
                            font=(UI_FONT, 10), wrap="word",
                            highlightbackground="#3c3c3c", highlightthickness=1)
        self.text.pack(padx=10, fill="x")
        self.text.insert("1.0", ann["text"])

        swatch_row = tk.Frame(self, bg=BG_PANEL)
        swatch_row.pack(anchor="w", padx=10, pady=8)
        self.color_var = tk.StringVar(value=ann["color"])
        self._swatches = {}
        for name, rgb in ANN_COLORS.items():
            c = tk.Canvas(swatch_row, width=22, height=22, bg=BG_PANEL,
                          highlightthickness=0, cursor="hand2")
            c.create_oval(3, 3, 19, 19, fill="#%02x%02x%02x" % rgb,
                          outline="", tags="dot")
            c.pack(side="left", padx=2)
            c.bind("<Button-1>", lambda e, nm=name: self._pick(nm))
            self._swatches[name] = c
        self._pick(ann["color"])

        if can_paste:
            self._button(self, "Insert into code as comment…", BG_ACTIVE, FG,
                         self._paste).pack(fill="x", padx=10, pady=(0, 8))

        btn_row = tk.Frame(self, bg=BG_PANEL)
        btn_row.pack(fill="x", padx=10, pady=(0, 10))
        self._button(btn_row, "Save", ACCENT, "#ffffff", self._save).pack(
            side="left", expand=True, fill="x", padx=(0, 4))
        if not is_new:
            self._button(btn_row, "Delete", "#6e2020", "#f0b0b0", self._delete).pack(
                side="left", expand=True, fill="x", padx=4)
        self._button(btn_row, "Cancel", BG_ACTIVE, FG, self._cancel).pack(
            side="left", expand=True, fill="x", padx=(4, 0))

        self.bind("<Escape>", lambda e: self._cancel())
        self.text.bind("<Control-Return>", lambda e: (self._save(), "break")[1])

        self.update_idletasks()
        w, h = self.winfo_reqwidth(), self.winfo_reqheight()
        x = max(0, min(x, self.winfo_screenwidth() - w - 10))
        y = max(0, min(y + 14, self.winfo_screenheight() - h - 50))
        self.geometry("+%d+%d" % (x, y))
        self.transient(master)
        self.grab_set()
        self.text.focus_set()
        # click outside the popup cancels it
        self.bind("<Button-1>", self._maybe_cancel_outside)

    @staticmethod
    def _button(parent, label, bg, fg, cmd):
        return tk.Button(parent, text=label, bg=bg, fg=fg, relief="flat",
                         activebackground=bg, activeforeground=fg,
                         font=(UI_FONT, 9), cursor="hand2", command=cmd)

    def _maybe_cancel_outside(self, event):
        if not (0 <= event.x <= self.winfo_width()
                and 0 <= event.y <= self.winfo_height()):
            self._cancel()

    def _pick(self, name):
        self.color_var.set(name)
        for nm, canvas in self._swatches.items():
            canvas.delete("ring")
            if nm == name:
                canvas.create_oval(1, 1, 21, 21, outline="#ffffff",
                                   width=2, tags="ring")

    def _save(self):
        self.ann["text"] = self.text.get("1.0", "end").strip()
        self.ann["color"] = self.color_var.get()
        self.result = "save"
        self._close()

    def _paste(self):
        self.ann["text"] = self.text.get("1.0", "end").strip()
        self.ann["color"] = self.color_var.get()
        self.result = "paste"
        self._close()

    def _delete(self):
        self.result = "delete"
        self._close()

    def _cancel(self):
        self.result = None
        self._close()

    def _close(self):
        self.grab_release()
        # an overrideredirect window that holds the grab AND keyboard focus
        # leaves focus on None when destroyed, deadening all key bindings —
        # hand focus back to the main window explicitly
        self.master.focus_set()
        self.destroy()


# -------------------------------------------------------------- workspace

class Workspace:
    """Where things live: the opened folder, the source base the annotation
    paths are relative to, and the parallel annotation tree."""

    def __init__(self, ann_root_arg=None):
        self.folder = None
        self.source_base = None
        self.ann_root = None
        self._ann_root_arg = (os.path.abspath(ann_root_arg)
                              if ann_root_arg else None)

    def open(self, folder):
        self.folder = os.path.abspath(folder)
        # annotation paths mirror the source tree relative to the git repo
        # root (when inside one) so the same tree serves the whole repo
        self.source_base = find_git_root(self.folder) or self.folder
        self.ann_root = self._ann_root_arg or os.path.join(self.source_base,
                                                           ANN_DIR_NAME)

    def rel_name(self, file_path):
        """Path of a source file relative to the base, with forward slashes."""
        return os.path.relpath(file_path,
                               self.source_base).replace(os.sep, "/")

    def sidecar_for(self, file_path):
        """Location of a source file's annotation file in the parallel tree."""
        rel = os.path.relpath(file_path, self.source_base)
        return os.path.join(self.ann_root, rel + SIDECAR_SUFFIX)

    def save(self, file_path, annotations):
        save_annotations(self.sidecar_for(file_path), self.rel_name(file_path),
                         annotations, prune_root=self.ann_root)


# ------------------------------------------------------------- folder tree

class FileTreePane:
    """Left-hand folder browser: lazy population, annotation badges."""

    def __init__(self, app, paned):
        self.app = app
        self.item_by_path = {}         # tree item id lookup for badges
        outer = tk.Frame(paned, bg=BG_PANEL)
        frame = tk.Frame(outer, bg=BG_PANEL)
        frame.pack(fill="both", expand=True)
        self.tree = ttk.Treeview(frame, show="tree", selectmode="browse",
                                 columns=("path", "kind"), displaycolumns=())
        scroll = ttk.Scrollbar(frame, orient="vertical",
                               command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")
        self.tree.bind("<<TreeviewOpen>>", self._on_open)
        self.tree.bind("<<TreeviewSelect>>", self._on_select)
        # pre-empt the Treeview class <Up>/<Down> bindings for Alt-arrows
        self.tree.bind("<Alt-Up>", lambda e: app._goto_annotation(-1))
        self.tree.bind("<Alt-Down>", lambda e: app._goto_annotation(1))

        # outline: every annotation in the open file, click to jump
        tk.Label(outer, text="Annotations", bg=BG_PANEL, fg=FG_DIM,
                 font=(UI_FONT, 9, "bold")).pack(anchor="w", padx=8,
                                                 pady=(6, 0))
        self.outline = tk.Listbox(outer, height=8, bg=BG_PANEL, fg=FG,
                                  relief="flat", highlightthickness=0,
                                  selectbackground=BG_ACTIVE,
                                  activestyle="none", font=(UI_FONT, 9))
        self.outline.pack(fill="x", padx=4, pady=(2, 6))
        self.outline.bind("<<ListboxSelect>>", self._on_outline_select)
        paned.add(outer, minsize=180, width=300)

    def refresh_outline(self, annotations):
        listbox = self.outline
        listbox.delete(0, "end")
        for a in annotations:
            rng = (str(a["startLine"]) if a["startLine"] == a["endLine"]
                   else f"{a['startLine']}–{a['endLine']}")
            note = " ".join(a["text"].split()) or "(no text)"
            prefix = "⚠ " if a.get("orphaned") else ""
            listbox.insert("end", f" {prefix}{rng}  {note[:60]}")
            rgb = ANN_COLORS[a["color"]]
            listbox.itemconfig("end", foreground="#%02x%02x%02x" % rgb)

    def _on_outline_select(self, _event):
        sel = self.outline.curselection()
        if sel:
            self.app.show_annotation(sel[0])

    def show_folder(self, folder):
        self.item_by_path.clear()
        self.tree.delete(*self.tree.get_children())
        root_item = self.tree.insert(
            "", "end", text=" \U0001F4C1 " + (os.path.basename(folder) or folder),
            values=[folder, "dir"], open=True)
        self._populate_dir(root_item, folder)

    def _populate_dir(self, item, path):
        """Fill one tree directory node with its real children."""
        workspace = self.app.workspace
        try:
            names = sorted(os.listdir(path),
                           key=lambda n: (os.path.isfile(os.path.join(path, n)),
                                          n.lower()))
        except OSError as exc:
            self.tree.insert(item, "end",
                             text=" ⚠ " + (exc.strerror or str(exc)),
                             values=["", "err"])
            return
        for name in names:
            full = os.path.join(path, name)
            if os.path.isdir(full):
                if name in SKIP_DIRS:
                    continue
                if os.path.abspath(full) == os.path.abspath(workspace.ann_root):
                    continue    # keep the annotation tree out of the browser
                child = self.tree.insert(item, "end",
                                         text=" \U0001F4C1 " + name,
                                         values=[full, "dir"])
                # dummy node so the expander arrow shows; filled lazily
                self.tree.insert(child, "end", text="…", values=["", "dummy"])
            else:
                is_sidecar = name.endswith(SIDECAR_SUFFIX)
                badge = " ●" if os.path.isfile(workspace.sidecar_for(full)) else ""
                icon = "\U0001F3F7" if is_sidecar else "\U0001F4C4"
                child = self.tree.insert(item, "end",
                                         text=" " + icon + " " + name + badge,
                                         values=[full, "file"])
                self.item_by_path[full] = child

    def _on_open(self, _event):
        item = self.tree.focus()
        children = self.tree.get_children(item)
        if len(children) == 1:
            values = self.tree.item(children[0], "values")
            if values and values[1] == "dummy":
                self.tree.delete(children[0])
                self._populate_dir(item, self.tree.item(item, "values")[0])

    def _on_select(self, _event):
        sel = self.tree.selection()
        if not sel:
            return
        values = self.tree.item(sel[0], "values")
        if values and values[1] == "file":
            if self.app.edit.active and not self.app.edit.exit():
                return
            self.app.open_file(values[0])

    def refresh_badge(self, file_path):
        item = self.item_by_path.get(file_path)
        if not item:
            return
        text = self.tree.item(item, "text")
        if text.endswith(" ●"):          # exact suffix — rstrip would also
            text = text[:-2]             # eat trailing ●/spaces in the NAME
        if os.path.isfile(self.app.workspace.sidecar_for(file_path)):
            text += " ●"
        self.tree.item(item, text=text)


# ----------------------------------------------------------- find / go-to

class FindController:
    """Find and go-to-line overlay bars for the annotate view, plus the
    match list and its navigation."""

    def __init__(self, app, parent):
        self.app = app
        self.matches = []              # find results as (line, col, length)
        self.match_idx = -1
        self._last_query = ""

        entry_opts = dict(bg=BG, fg=FG, insertbackground=FG, relief="flat",
                          font=(UI_FONT, 10),
                          highlightbackground="#3c3c3c", highlightthickness=1)
        btn_opts = dict(bg=BG_PANEL, fg=FG, relief="flat", cursor="hand2",
                        activebackground=BG_ACTIVE, activeforeground=FG,
                        font=(UI_FONT, 10), padx=6)

        self.find_bar = tk.Frame(parent, bg=BG_PANEL,
                                 highlightbackground="#3c3c3c",
                                 highlightthickness=1)
        self.find_entry = tk.Entry(self.find_bar, width=24, **entry_opts)
        self.find_entry.pack(side="left", padx=(8, 4), pady=5)
        self.find_count = tk.Label(self.find_bar, text="", width=9,
                                   bg=BG_PANEL, fg=FG_DIM,
                                   font=(UI_FONT, 9))
        self.find_count.pack(side="left")
        for label, cmd in (("‹", self.prev), ("›", self.next),
                           ("✕", app._close_bars)):
            tk.Button(self.find_bar, text=label, command=cmd,
                      **btn_opts).pack(side="left", padx=(0, 2))
        self.find_entry.bind("<KeyRelease>", self._on_key)
        self.find_entry.bind("<Return>", self.next)
        self.find_entry.bind("<Shift-Return>", self.prev)
        self.find_entry.bind("<Escape>", app._close_bars)

        self.goto_bar = tk.Frame(parent, bg=BG_PANEL,
                                 highlightbackground="#3c3c3c",
                                 highlightthickness=1)
        tk.Label(self.goto_bar, text="Go to line:", bg=BG_PANEL, fg=FG_DIM,
                 font=(UI_FONT, 9)).pack(side="left", padx=(8, 4))
        self.goto_entry = tk.Entry(self.goto_bar, width=8, **entry_opts)
        self.goto_entry.pack(side="left", padx=(0, 8), pady=5)
        self.goto_entry.bind("<Return>", self.goto_line)
        self.goto_entry.bind("<Escape>", app._close_bars)

    def visible(self):
        return (self.find_bar.winfo_ismapped()
                or self.goto_bar.winfo_ismapped())

    def open_find(self, _event=None):
        app = self.app
        if app.edit.active or not app.file_path:
            return "break"
        self.goto_bar.place_forget()
        self.find_bar.place(relx=1.0, x=-24, y=8, anchor="ne")
        self.find_entry.focus_set()
        self.find_entry.select_range(0, "end")
        self.refresh()
        return "break"

    def open_goto(self, _event=None):
        app = self.app
        if app.edit.active or not app.file_path:
            return "break"
        self.find_bar.place_forget()
        self.goto_bar.place(relx=1.0, x=-24, y=8, anchor="ne")
        self.goto_entry.delete(0, "end")
        self.goto_entry.focus_set()
        return "break"

    def close_bars(self):
        """Hide both bars and drop match state (dismissal is coordinated by
        the app, which also clears selection and paste placement)."""
        self.find_bar.place_forget()
        self.goto_bar.place_forget()
        self.reset()
        self._last_query = ""
        self.app.viewer.canvas.delete("find")

    def reset(self):
        self.matches, self.match_idx = [], -1

    def on_file_opened(self):
        if self.find_bar.winfo_ismapped():
            self.refresh()               # re-run the search in the new file
        else:
            self.reset()

    def _on_key(self, _event=None):
        if self.find_entry.get() != self._last_query:
            self.refresh()

    def refresh(self):
        app = self.app
        query = self.find_entry.get()
        self._last_query = query
        self.reset()
        app.viewer.canvas.delete("find")
        if query and app.file_path:
            q = query.lower()
            for row, line in enumerate(app.lines):
                low, start = line.lower(), 0
                while len(self.matches) < 2000:
                    col = low.find(q, start)
                    if col < 0:
                        break
                    self.matches.append((row + 1, col, len(query)))
                    start = col + len(q)
                if len(self.matches) >= 2000:
                    break
        if self.matches:
            self.goto_match(0)
        else:
            self.find_count.config(text="no results" if query else "")

    def draw(self):
        app = self.app
        canvas = app.viewer.canvas
        canvas.delete("find")
        for idx, (ln, col, length) in enumerate(self.matches):
            x0 = app.pad + col * app.char_w
            y0 = app.pad + (ln - 1) * app.line_h
            current = idx == self.match_idx
            canvas.create_rectangle(
                x0 - 1, y0 + 1, x0 + length * app.char_w + 1,
                y0 + app.line_h - 1,
                fill=blend(BG, (210, 170, 60), 0.5 if current else 0.25),
                outline="#d7ba7d" if current else "", tags="find")
        if canvas.find_withtag("find") and canvas.find_withtag("code"):
            canvas.tag_lower("find", "code")
            if canvas.find_withtag("ann"):
                canvas.tag_raise("find", "ann")

    def goto_match(self, idx):
        app = self.app
        if app.edit.active or not self.matches:
            return "break"
        self.match_idx = idx % len(self.matches)
        self.draw()
        ln, col, _length = self.matches[self.match_idx]
        viewer = app.viewer
        viewer.scroll_to_line(ln)
        x0 = app.pad + col * app.char_w
        left = viewer.canvas.canvasx(0)
        view_w = viewer.canvas.winfo_width()
        if not (left <= x0 <= left + view_w - 80):
            viewer.canvas.xview_moveto(
                max(0.0, (x0 - view_w / 3.0) / max(1, viewer.content_width)))
        self.find_count.config(text="%d of %d" % (self.match_idx + 1,
                                                  len(self.matches)))
        return "break"

    def next(self, _event=None):
        return self.goto_match(self.match_idx + 1)

    def prev(self, _event=None):
        return self.goto_match(self.match_idx - 1)

    def goto_line(self, _event=None):
        app = self.app
        try:
            ln = int(self.goto_entry.get().strip())
        except ValueError:
            return "break"
        ln = max(1, min(len(app.lines), ln))
        self.goto_bar.place_forget()
        app.viewer.scroll_to_line(ln)
        app.viewer.set_selection((ln, ln))
        app.viewer.canvas.focus_set()
        return "break"


# ---------------------------------------------------------------- edit mode

class EditorPane:
    """Edit mode: a real Text editor (undo/redo, syntax tags, annotation
    bands that ride along with the text) swapped in for the canvas view."""

    def __init__(self, app, view):
        self.app = app
        self.active = False            # editor swapped in for the canvas view
        self.dirty = False             # unsaved editor changes
        self._newline = "\n"           # newline style of the file being edited
        self._line_tokens = []         # per-line token cache for re-highlight
        self._hl_after = None          # pending debounced highlight callback
        self._gutter_after = None      # pending gutter redraw callback
        self._mtime = None             # source mtime at enter/last save
        self._find_hits = []           # in-editor find matches (indices)
        self._find_pos = -1

        self.frame = tk.Frame(view, bg=BG)
        self.gutter = tk.Canvas(self.frame, width=56, bg=BG,
                                highlightthickness=0)
        self.gutter.pack(side="left", fill="y")
        self.editor = tk.Text(self.frame, bg=BG, fg=FG,
                              insertbackground=FG, selectbackground="#264f78",
                              relief="flat", borderwidth=0, wrap="none",
                              undo=True, maxundo=-1, font=app.code_font,
                              tabs=str(TAB_WIDTH * app.char_w))
        self._vbar = ttk.Scrollbar(self.frame, orient="vertical",
                                   command=self.editor.yview)
        hbar = ttk.Scrollbar(self.frame, orient="horizontal",
                             command=self.editor.xview)
        self.editor.configure(yscrollcommand=self._on_yscroll,
                              xscrollcommand=hbar.set)
        self._vbar.pack(side="right", fill="y")
        hbar.pack(side="bottom", fill="x")
        self.editor.pack(side="left", fill="both", expand=True)
        for cls, color in TOKEN_COLORS.items():
            if cls:
                self.editor.tag_configure("tok_" + cls, foreground=color)
        self.editor.bind("<<Modified>>", self._on_modified)
        self.editor.bind("<Configure>", lambda e: self._schedule_gutter())
        bind_mousewheel(self.editor, self._wheel)
        bind_mousewheel(self.gutter, self._wheel)
        self.editor.bind("<Control-Shift-Z>", self._redo)
        self.editor.bind("<Control-s>", self.save)
        # Ctrl+F opens the in-editor search (and pre-empts the Text class's
        # emacs-style cursor motion); Ctrl+G stays reserved
        self.editor.bind("<Control-f>", self.open_find)
        self.editor.bind("<Control-g>", lambda e: "break")

        # --- in-editor find bar ---
        self.editor.tag_configure("efind",
                                  background=blend(BG, (210, 170, 60), 0.25))
        self.editor.tag_configure("efindcur",
                                  background=blend(BG, (210, 170, 60), 0.55))
        self._find_bar = tk.Frame(self.frame, bg=BG_PANEL,
                                  highlightbackground="#3c3c3c",
                                  highlightthickness=1)
        self._find_entry = tk.Entry(self._find_bar, width=24, bg=BG, fg=FG,
                                    insertbackground=FG, relief="flat",
                                    font=(UI_FONT, 10),
                                    highlightbackground="#3c3c3c",
                                    highlightthickness=1)
        self._find_entry.pack(side="left", padx=(8, 4), pady=5)
        self._find_count = tk.Label(self._find_bar, text="", width=9,
                                    bg=BG_PANEL, fg=FG_DIM,
                                    font=(UI_FONT, 9))
        self._find_count.pack(side="left")
        for label, cmd in (("‹", self.find_prev), ("›", self.find_next),
                           ("✕", self.close_find)):
            tk.Button(self._find_bar, text=label, command=cmd, bg=BG_PANEL,
                      fg=FG, relief="flat", cursor="hand2",
                      activebackground=BG_ACTIVE, activeforeground=FG,
                      font=(UI_FONT, 10), padx=6).pack(side="left",
                                                       padx=(0, 2))
        self._find_entry.bind("<KeyRelease>", self._find_on_key)
        self._find_entry.bind("<Return>", self.find_next)
        self._find_entry.bind("<Shift-Return>", self.find_prev)
        self._find_entry.bind("<Escape>", self.close_find)

    def toggle(self):
        if self.active:
            self.exit()
        else:
            self.enter()

    def enter(self):
        app = self.app
        if self.active:
            return
        if not app.file_path:
            app._flash_status("Open a file first")
            return
        if app._truncated:
            app._flash_status("File is truncated in this view — too large "
                              "to edit here")
            return
        try:
            with open(app.file_path, "rb") as f:
                raw = f.read()
            content = raw.decode("utf-8")
        except OSError as exc:
            app._flash_status("Could not read file: %s" % exc)
            return
        except UnicodeDecodeError:
            app._flash_status("File is not valid UTF-8 — editing disabled")
            return
        app._close_bars()                # drop find/selection/paste state
        try:
            self._mtime = os.path.getmtime(app.file_path)
        except OSError:
            self._mtime = None
        self._newline = "\r\n" if b"\r\n" in raw else "\n"
        content = content.replace("\r\n", "\n").replace("\r", "\n")

        self.editor.delete("1.0", "end")
        self.editor.insert("1.0", content)
        self.editor.edit_reset()         # no undo step back to emptiness
        self.editor.edit_modified(False)
        self.dirty = False
        self._line_tokens = []
        self.active = True

        app.viewer.hide()
        self.frame.pack(fill="both", expand=True)
        app.edit_btn.config(text="✓ Done")
        app.save_btn.pack(side="left", padx=(0, 4), pady=5,
                          after=app.edit_btn)
        self._apply_highlight()
        self._apply_bands()
        self._schedule_gutter()
        self.editor.mark_set("insert", "1.0")
        self.editor.focus_set()
        self._update_title()
        self._set_hint()

    def exit(self):
        """Leave edit mode; returns False if the user cancelled."""
        app = self.app
        if not self.active:
            return True
        if self.dirty:
            from tkinter import messagebox
            resp = messagebox.askyesnocancel(
                "Unsaved changes",
                "Save changes to %s?" % os.path.basename(app.file_path))
            if resp is None:
                return False
            if resp:
                self.save()
        self.close_find()
        self.active = False
        self.dirty = False
        self.frame.pack_forget()
        app.viewer.show()
        app.edit_btn.config(text="✎ Edit")
        app.save_btn.pack_forget()
        app.open_file(app.file_path)     # re-render the (possibly new) source
        return True

    def save(self, _event=None):
        app = self.app
        if not self.active:
            return
        try:
            on_disk = os.path.getmtime(app.file_path)
        except OSError:
            on_disk = self._mtime
        if self._mtime is not None and on_disk != self._mtime:
            from tkinter import messagebox
            if not messagebox.askyesno(
                    "File changed on disk",
                    "%s was modified outside the editor.\n\nOverwrite it "
                    "with your version?" % os.path.basename(app.file_path)):
                return "break"
        text = self.editor.get("1.0", "end-1c")
        if self._newline != "\n":
            text = text.replace("\n", self._newline)
        # atomic save: opening the target with "wb" would truncate it before
        # writing, so a crash mid-save could destroy the source file
        tmp = app.file_path + ".tmp~"
        try:
            with open(tmp, "wb") as f:
                f.write(text.encode("utf-8"))
            os.replace(tmp, app.file_path)
        except OSError as exc:
            try:
                os.remove(tmp)
            except OSError:
                pass
            from tkinter import messagebox
            messagebox.showerror("Save failed",
                                 "Could not write the source file:\n%s" % exc)
            return "break"
        try:
            self._mtime = os.path.getmtime(app.file_path)
        except OSError:
            self._mtime = None
        self._sync_annotations()
        self.dirty = False
        self.editor.edit_modified(False)
        self._update_title()
        app._flash_status("Saved %s" % os.path.basename(app.file_path))
        return "break"

    def _sync_annotations(self):
        """Re-anchor annotations from their band tags, which move with the
        text as it is edited, then persist any changes."""
        app = self.app
        changed = False
        for a in app.annotations:
            rng = self.editor.tag_ranges("eband_" + a["id"])
            if not rng:
                # the whole annotated region was deleted — collapse the
                # annotation to the deletion point (tracked by its mark)
                # and flag it as orphaned instead of keeping stale lines
                mark = "eann_" + a["id"]
                if mark not in self.editor.mark_names():
                    continue
                ln = int(self.editor.index(mark).split(".")[0])
                total = int(self.editor.index("end-1c").split(".")[0])
                ln = max(1, min(ln, total))
                if (a["startLine"], a["endLine"],
                        bool(a.get("orphaned"))) != (ln, ln, True):
                    a["startLine"], a["endLine"] = ln, ln
                    a["orphaned"] = True
                    changed = True
                continue
            start = int(str(rng[0]).split(".")[0])
            end_ln, end_col = (int(v) for v in str(rng[-1]).split("."))
            if end_col == 0:             # band ends at a line start: exclusive
                end_ln -= 1
            end_ln = max(start, end_ln)
            anchor = anchor_text(self.editor.get(f"{start}.0", f"{start}.end"))
            if (start, end_ln, anchor) != (a["startLine"], a["endLine"],
                                           a.get("anchor")):
                a["startLine"], a["endLine"] = start, end_ln
                a["anchor"] = anchor
                changed = True
        if changed:
            app.annotations.sort(key=lambda a: a["startLine"])
            app._persist()

    def _apply_bands(self):
        """Show annotation ranges as background tags in the editor, and drop
        a mark at each start — marks survive deletion of the surrounding
        text, so a fully deleted region still has a trackable position."""
        app = self.app
        for mark in self.editor.mark_names():
            if mark.startswith("eann_"):     # stale marks from earlier files
                self.editor.mark_unset(mark)
        total = int(self.editor.index("end-1c").split(".")[0])
        for a in app.annotations:
            tag = "eband_" + a["id"]
            self.editor.tag_configure(tag, background=self._band_color(a))
            start = min(a["startLine"], total)
            end = min(a["endLine"], total)
            self.editor.tag_add(tag, "%d.0" % start, "%d.0" % (end + 1))
            self.editor.tag_lower(tag)
            self.editor.mark_set("eann_" + a["id"], "%d.0" % start)
            self.editor.mark_gravity("eann_" + a["id"], "left")
        self.editor.tag_raise("sel")     # selection stays visible over bands

    def _band_color(self, a):
        """Band background for one annotation ('' = hidden by color filter).
        The tag itself always exists — it tracks the region for save."""
        if a["color"] not in self.app.visible_colors:
            return ""
        alpha = self.app.opacity.get() / 100.0
        factor = 0.18 if a.get("orphaned") else 0.45
        return blend(BG, ANN_COLORS[a["color"]], alpha * factor)

    def update_band_colors(self):
        for a in self.app.annotations:
            self.editor.tag_configure("eband_" + a["id"],
                                      background=self._band_color(a))

    # --- in-editor find ---

    def open_find(self, _event=None):
        if not self.active:
            return "break"
        self._find_bar.place(relx=1.0, x=-24, y=8, anchor="ne")
        self._find_entry.focus_set()
        self._find_entry.select_range(0, "end")
        self._find_refresh()
        return "break"

    def close_find(self, _event=None):
        self._find_bar.place_forget()
        self.editor.tag_remove("efind", "1.0", "end")
        self.editor.tag_remove("efindcur", "1.0", "end")
        self._find_hits, self._find_pos = [], -1
        if self.active:
            self.editor.focus_set()

    def _find_on_key(self, _event=None):
        self._find_refresh()

    def _find_refresh(self):
        editor = self.editor
        editor.tag_remove("efind", "1.0", "end")
        editor.tag_remove("efindcur", "1.0", "end")
        self._find_hits, self._find_pos = [], -1
        query = self._find_entry.get()
        if query:
            idx = "1.0"
            while len(self._find_hits) < 2000:
                idx = editor.search(query, idx, stopindex="end", nocase=True)
                if not idx:
                    break
                end = f"{idx}+{len(query)}c"
                editor.tag_add("efind", idx, end)
                self._find_hits.append(idx)
                idx = end
        if self._find_hits:
            self.find_next()
        else:
            self._find_count.config(text="no results" if query else "")

    def find_next(self, _event=None, step=1):
        if not self.active:
            return "break"
        if not self._find_hits:
            return "break"
        self._find_pos = (self._find_pos + step) % len(self._find_hits)
        idx = self._find_hits[self._find_pos]
        length = len(self._find_entry.get())
        self.editor.tag_remove("efindcur", "1.0", "end")
        self.editor.tag_add("efindcur", idx, f"{idx}+{length}c")
        self.editor.mark_set("insert", idx)
        self.editor.see(idx)
        self._find_count.config(text=f"{self._find_pos + 1} of "
                                     f"{len(self._find_hits)}")
        return "break"

    def find_prev(self, _event=None):
        return self.find_next(step=-1)

    def on_zoom(self):
        # the Text widget follows the shared named font by itself
        self.editor.configure(tabs=str(TAB_WIDTH * self.app.char_w))
        self._schedule_gutter()

    def _set_hint(self):
        self.app.status_hint.config(
            text="Editing • Ctrl+Z undo • Ctrl+Y redo • Ctrl+S save • "
                 "Done returns to the annotate view")

    def _update_title(self):
        app = self.app
        if self.active and app.file_path:
            app.status_file.config(
                text="%s  •  editing%s" % (os.path.basename(app.file_path),
                                           "  ●" if self.dirty else ""))

    def _on_modified(self, _event=None):
        if not self.editor.edit_modified():
            return
        self.editor.edit_modified(False)   # re-arm the <<Modified>> event
        if not self.active:
            return                         # programmatic load/teardown
        self.dirty = True
        self._update_title()
        self._schedule_highlight()
        self._schedule_gutter()
        if self._find_bar.winfo_ismapped():
            self._find_refresh()         # match indices go stale on edits

    def _redo(self, _event=None):
        try:
            self.editor.edit_redo()
        except tk.TclError:
            pass                           # nothing to redo
        return "break"

    def _wheel(self, event):
        step = wheel_steps(event)
        if event.state & 0x0004:
            return self.app._zoom(step)
        self.editor.yview_scroll(-3 * step, "units")
        return "break"

    def _on_yscroll(self, first, last):
        self._vbar.set(first, last)
        self._schedule_gutter()

    def _schedule_gutter(self):
        if self._gutter_after is None:
            self._gutter_after = self.app.root.after_idle(self._redraw_gutter)

    def _redraw_gutter(self):
        self._gutter_after = None
        app = self.app
        gutter = self.gutter
        gutter.delete("all")
        if not self.active:
            return
        total = int(self.editor.index("end-1c").split(".")[0])
        gutter_w = app.pad * 2 + max(2, len(str(total))) * app.char_w
        gutter.configure(width=gutter_w)
        ln = int(self.editor.index("@0,0").split(".")[0])
        while ln <= total:
            dli = self.editor.dlineinfo("%d.0" % ln)
            if dli is None:
                break
            gutter.create_text(gutter_w - app.pad, dli[1], anchor="ne",
                               text=str(ln), font=app.code_font,
                               fill=GUTTER_FG)
            ln += 1

    def _schedule_highlight(self):
        if self._hl_after is not None:
            self.app.root.after_cancel(self._hl_after)
        self._hl_after = self.app.root.after(150, self._apply_highlight)

    def _apply_highlight(self):
        """Re-tokenize the editor text and retag only the changed lines."""
        self._hl_after = None
        lines = self.editor.get("1.0", "end-1c").split("\n")
        tokenized = highlight_lines(lines, self.app.lang_id)
        old = self._line_tokens
        add = {}                           # token class -> flat index list
        dirty = []                         # 1-based lines needing a retag
        for i, tokens in enumerate(tokenized):
            if i < len(old) and old[i] == tokens:
                continue
            dirty.append(i + 1)
            for col, run, cls in tokens:
                if cls:
                    add.setdefault(cls, []).extend(
                        ("%d.%d" % (i + 1, col),
                         "%d.%d" % (i + 1, col + len(run))))
        if dirty:
            spans, lo = [], dirty[0]
            prev = lo
            for ln in dirty[1:]:
                if ln != prev + 1:
                    spans.append((lo, prev))
                    lo = ln
                prev = ln
            spans.append((lo, prev))
            for cls in TOKEN_COLORS:
                if not cls:
                    continue
                for s, e in spans:
                    self.editor.tag_remove("tok_" + cls,
                                           "%d.0" % s, "%d.0" % (e + 1))
        for cls, idxs in add.items():
            for k in range(0, len(idxs), 2048):
                self.editor.tag_add("tok_" + cls, *idxs[k:k + 2048])
        self._line_tokens = tokenized


# ------------------------------------------------------------- annotate view

class ViewerPane:
    """The annotate view: syntax-colored code rendered on a canvas, the
    line-number gutter, translucent annotation bands with word-wrapped
    notes, line selection/copy, and the gutter's annotate gestures."""

    def __init__(self, app, view):
        self.app = app
        self.content_width = 0
        self._tokenized = []           # cached tokens so zoom can re-render
        self._line_cols = []           # per-line code width, in columns
        self._sel = None               # selected line range (lo, hi)
        self._sel_anchor = None        # code-area drag anchor line
        self._dragged = False          # True once a code-area drag moves
        self._drag_start = None        # gutter drag anchor line
        self._pending = None           # (lo, hi) line range being selected

        self.gutter = tk.Canvas(view, width=56, bg=BG, highlightthickness=0,
                                cursor="hand2")
        self.gutter.pack(side="left", fill="y")
        self.gutter.bind("<Button-1>", self._gutter_press)
        self.gutter.bind("<B1-Motion>", self._gutter_drag)
        self.gutter.bind("<ButtonRelease-1>", self._gutter_release)

        self.code_frame = tk.Frame(view, bg=BG)
        self.code_frame.pack(side="left", fill="both", expand=True)
        self.canvas = tk.Canvas(self.code_frame, bg=BG, highlightthickness=0)
        vbar = ttk.Scrollbar(self.code_frame, orient="vertical",
                             command=self.yview)
        self.hbar = ttk.Scrollbar(self.code_frame, orient="horizontal",
                                  command=self.canvas.xview)
        self.canvas.configure(yscrollcommand=vbar.set,
                              xscrollcommand=self._on_xscroll)
        vbar.pack(side="right", fill="y")
        self.hbar.pack(side="bottom", fill="x")
        self.canvas.pack(side="left", fill="both", expand=True)

        for widget in (self.canvas, self.gutter):
            bind_mousewheel(widget, self._on_wheel)
            widget.bind("<Up>", lambda e: self._scroll_lines(-1))
            widget.bind("<Down>", lambda e: self._scroll_lines(1))
            widget.bind("<Prior>", lambda e: self._scroll_pages(-1))
            widget.bind("<Next>", lambda e: self._scroll_pages(1))
            widget.bind("<Home>", lambda e: self._scroll_home_end(0))
            widget.bind("<End>", lambda e: self._scroll_home_end(1))
            widget.bind("<Alt-Up>", lambda e: app._goto_annotation(-1))
            widget.bind("<Alt-Down>", lambda e: app._goto_annotation(1))
        self.canvas.bind("<Configure>", lambda e: self.draw_annotations())
        self.canvas.tag_bind("ann", "<ButtonRelease-1>", self._on_ann_click)
        self.canvas.bind("<Button-1>", self._code_press)
        self.canvas.bind("<B1-Motion>", self._code_drag)
        self.canvas.bind("<ButtonRelease-1>", self._code_release)

    # -------------------------------------------------------------- lifecycle

    def hide(self):
        """Swap out for the editor pane."""
        self.gutter.pack_forget()
        self.code_frame.pack_forget()

    def show(self):
        self.gutter.pack(side="left", fill="y")
        self.code_frame.pack(side="left", fill="both", expand=True)

    def reset_interaction(self):
        self._pending = None
        self._drag_start = None
        self._sel = None
        self._sel_anchor = None

    def clear(self, msg=None):
        """Empty the view, optionally leaving a centered message."""
        self._tokenized = []
        self._line_cols = []
        self.reset_interaction()
        self.canvas.delete("all")
        self.gutter.delete("all")
        if msg:
            self.canvas.create_text(
                max(360, self.canvas.winfo_width() // 2), 120,
                fill=FG_DIM, font=(UI_FONT, 11), text=msg)

    def show_welcome(self):
        self.canvas.delete("all")
        cx = max(360, self.canvas.winfo_width() // 2)
        self.canvas.create_text(
            cx, 120, fill=FG, font=(UI_FONT, 20), tags="welcome",
            text="Code Annotator")
        self.canvas.create_text(
            cx, 190, fill=FG_DIM, font=(UI_FONT, 11), justify="center",
            tags="welcome",
            text="Open a project folder, pick a code file, and see its\n"
                 "annotations overlaid transparently on the source.\n\n"
                 "Click or drag on the line numbers to add an annotation.\n"
                 "Click an existing highlight to edit it.\n\n"
                 "Ctrl+F finds text, Ctrl+G jumps to a line, Ctrl+C copies\n"
                 "selected lines, Ctrl+wheel or Ctrl+=/- zooms the code,\n"
                 "Alt+Down/Alt+Up jump to the next/previous annotation.\n\n"
                 "Notes are saved in a parallel tree that mirrors the\n"
                 "source layout (default: <project>/" + ANN_DIR_NAME + "),\n"
                 "so the source files themselves are never touched.")

    # -------------------------------------------------------------- rendering

    def render(self, tokenized):
        """Draw a freshly tokenized file (also cached for zoom re-renders)."""
        self._tokenized = tokenized
        self.reset_interaction()
        self._render_code(tokenized)

    def on_zoom(self):
        app = self.app
        if not app.file_path:
            return
        first = self.canvas.yview()[0]
        self._render_code(self._tokenized)
        self.draw_annotations()
        app.finder.draw()
        self.set_selection(self._sel)
        self.yview("moveto", first)

    def _render_code(self, tokenized):
        app = self.app
        canvas, lh = self.canvas, app.line_h
        canvas.delete("all")
        self.gutter.delete("all")

        max_cols = 0
        self._line_cols = []
        for row, tokens in enumerate(tokenized):
            y = app.pad + row * lh
            for col, run, cls in tokens:
                if not run.strip() and cls is None:
                    continue
                canvas.create_text(app.pad + col * app.char_w, y,
                                   anchor="nw", text=run,
                                   font=app.code_font,
                                   fill=TOKEN_COLORS.get(cls, FG),
                                   tags="code")
            cols = tokens[-1][0] + len(tokens[-1][1]) if tokens else 0
            self._line_cols.append(cols)
            max_cols = max(max_cols, cols)

        n = len(app.lines)
        self.content_width = app.pad * 2 + max_cols * app.char_w + 300
        height = app.pad * 2 + n * lh
        canvas.configure(scrollregion=(0, 0, self.content_width, height),
                         yscrollincrement=lh)

        gutter_w = app.pad * 2 + max(2, len(str(n))) * app.char_w
        for row in range(n):
            self.gutter.create_text(gutter_w - app.pad, app.pad + row * lh,
                                    anchor="ne", text=str(row + 1),
                                    font=app.code_font, fill=GUTTER_FG,
                                    tags="ln_%d" % (row + 1))
        self.gutter.configure(width=gutter_w, yscrollincrement=lh,
                              scrollregion=(0, 0, gutter_w, height))
        self.yview("moveto", 0)
        self.canvas.xview_moveto(0)

    # ----------------------------------------------------- scrolling plumbing

    def yview(self, *args):
        self.canvas.yview(*args)
        self.gutter.yview(*args)

    def _on_wheel(self, event):
        step = wheel_steps(event)
        if event.state & 0x0004:        # Ctrl held — zoom instead of scroll
            return self.app._zoom(step)
        self.canvas.yview_scroll(-step * 3, "units")
        self.gutter.yview_scroll(-step * 3, "units")
        return "break"

    def _on_xscroll(self, first, last):
        # notes hug the right edge of the viewport and wrap to clear the
        # code, so any horizontal viewport shift means a re-wrap
        self.hbar.set(first, last)
        self.draw_annotations()

    def _scroll_lines(self, n):
        self.canvas.yview_scroll(n, "units")
        self.gutter.yview_scroll(n, "units")
        return "break"

    def _scroll_pages(self, n):
        self.canvas.yview_scroll(n, "pages")
        self.gutter.yview_scroll(n, "pages")
        return "break"

    def _scroll_home_end(self, frac):
        self.yview("moveto", frac)
        return "break"

    def scroll_to_line(self, ln):
        """Scroll vertically (if needed) so line ``ln`` is comfortably visible."""
        app = self.app
        y = app.pad + (ln - 1) * app.line_h
        top = self.canvas.canvasy(0)
        view_h = self.canvas.winfo_height()
        if not (top + app.line_h <= y <= top + view_h - 2 * app.line_h):
            total = app.pad * 2 + len(app.lines) * app.line_h
            self.yview("moveto", max(0.0, (y - view_h / 3.0) / total))

    def line_visible(self, ln):
        app = self.app
        y = app.pad + (ln - 1) * app.line_h
        top = self.canvas.canvasy(0)
        return top <= y <= top + self.canvas.winfo_height() - app.line_h

    def viewport_lines(self):
        """(top, bottom) 1-based line numbers of the visible region."""
        app = self.app
        top = int((self.canvas.canvasy(0) - app.pad) // app.line_h) + 1
        return top, top + self.canvas.winfo_height() // app.line_h

    # -------------------------------------------------------- overlay drawing

    def _note_x(self):
        return self.canvas.canvasx(self.canvas.winfo_width()) - 12

    def draw_annotations(self, *_args):
        app = self.app
        canvas, lh = self.canvas, app.line_h
        canvas.delete("ann")
        canvas.delete("pending")
        if not app.file_path:
            return

        alpha = app.opacity.get() / 100.0
        if app.show_annotations.get():
            for a in app.annotations:
                if a["color"] not in app.visible_colors:
                    continue             # hidden by the toolbar color filter
                rgb = ANN_COLORS[a["color"]]
                orphaned = a.get("orphaned")
                y0 = app.pad + (a["startLine"] - 1) * lh
                y1 = app.pad + min(a["endLine"], len(app.lines)) * lh
                band = blend(BG, rgb, alpha * (0.18 if orphaned else 0.45))
                edge = blend(BG, rgb, min(1.0, alpha * 1.5))
                tag = ("ann", "ann_" + a["id"])
                canvas.create_rectangle(0, y0, self.content_width, y1,
                                        fill=band, outline="", tags=tag)
                if orphaned:             # dashed edge marks a best-guess spot
                    canvas.create_line(2, y0 + 1, 2, y1 - 1, fill=edge,
                                       width=3, dash=(3, 3), tags=tag)
                else:
                    canvas.create_rectangle(0, y0, 3, y1, fill=edge,
                                            outline="", tags=tag)
                if a["text"]:
                    self._draw_note(a, y0, tag, alpha)
            # highlight bands sit under the code so text stays crisp;
            # note text stays on top (tag_lower/raise error out when the
            # reference tag matches nothing, e.g. a file of blank lines)
            if canvas.find_withtag("code"):
                canvas.tag_lower("ann", "code")
            canvas.tag_raise("note")
            if canvas.find_withtag("find") and canvas.find_withtag("ann"):
                canvas.tag_raise("find", "ann")   # search hits over the bands

        if self._pending:
            lo, hi = self._pending
            y0 = app.pad + (lo - 1) * lh
            y1 = app.pad + hi * lh
            canvas.create_rectangle(0, y0, self.content_width, y1,
                                    fill=blend(BG, (14, 99, 156), 0.35),
                                    outline="", tags="pending")
            if canvas.find_withtag("code"):
                canvas.tag_lower("pending", "code")

        count = len(app.annotations)
        hint = ("Click or drag on line numbers to annotate • "
                "click a highlight to edit • Ctrl+F to search")
        if count:
            hint = "%d annotation%s  •  %s" % (count,
                                               "s" if count != 1 else "", hint)
        # don't clobber the placement prompt, a transient confirmation,
        # or the edit-mode hint
        if not (app._paste_ann or app._flash_msg or app.edit.active):
            app.status_hint.config(text=hint)

    def _draw_note(self, a, y0, tag, alpha):
        """Draw one note, word-wrapped so it stays clear of the code.

        The wrap width is the gap between the right edge of the code and the
        right edge of the viewport, over the lines the note vertically covers.
        Wrapping narrower makes the note taller (covering more code lines), so
        the cleared range is grown iteratively until the layout settles. When
        the gap is hopeless the width is clamped and the note may overlap."""
        app = self.app
        nx = self._note_x()
        lo = a["startLine"]
        gap, min_w = 24, 80

        def avail(hi):
            hi = max(lo, min(hi, len(self._line_cols)))
            cols = max(self._line_cols[lo - 1:hi] or [0])
            return int(nx - gap - (app.pad + cols * app.char_w))

        hi_cover = min(a["endLine"], len(app.lines))
        width = max(min_w, avail(hi_cover))
        note_fill = blend(BG, (255, 255, 255), min(1.0, 0.35 + alpha * 0.65))
        prefix = ("⚠ orphaned — " if a.get("orphaned")
                  else "\U0001F3F7 ")
        item = self.canvas.create_text(
            nx, y0 + 2, anchor="ne", width=width, justify="right",
            text=prefix + a["text"], font=app.note_font,
            fill=note_fill, tags=tag + ("note",))
        for _ in range(6):
            bottom = self.canvas.bbox(item)[3]
            covered = int((bottom - app.pad) // app.line_h) + 1
            if covered <= hi_cover or hi_cover >= len(app.lines):
                break
            hi_cover = min(covered, len(app.lines))
            new_w = max(min_w, avail(hi_cover))
            if new_w >= width:
                break
            width = new_w
            self.canvas.itemconfig(item, width=new_w)

    def flash_annotation(self, a):
        """Briefly outline an annotation's band so the jump target is obvious."""
        app = self.app
        y0 = app.pad + (a["startLine"] - 1) * app.line_h
        y1 = app.pad + min(a["endLine"], len(app.lines)) * app.line_h
        rgb = ANN_COLORS[a["color"]]
        rect = self.canvas.create_rectangle(
            1, y0, self.content_width - 1, y1,
            outline="#%02x%02x%02x" % rgb, width=2)
        app.root.after(700, lambda: self.canvas.delete(rect))

    # -------------------------------------------------- line selection / copy

    def _canvas_line(self, event):
        app = self.app
        y = self.canvas.canvasy(event.y) - app.pad
        return max(1, min(len(app.lines), int(y // app.line_h) + 1))

    def _code_press(self, event):
        app = self.app
        if not app.file_path:
            return
        self.canvas.focus_set()
        if app._paste_ann:
            self._dragged = True         # swallow the matching release/click
            app._paste_comment_at(self._canvas_line(event))
            return
        self._dragged = False
        self._sel_anchor = self._canvas_line(event)

    def _code_drag(self, event):
        if self._sel_anchor is None:
            return
        line = self._canvas_line(event)
        if line != self._sel_anchor:
            self._dragged = True
        if self._dragged:
            self.set_selection((min(self._sel_anchor, line),
                                max(self._sel_anchor, line)))

    def _code_release(self, event):
        if self._sel_anchor is None:
            return
        self._sel_anchor = None
        if self._dragged:
            return
        line = self._canvas_line(event)
        if (event.state & 0x0001) and self._sel:      # Shift-click extends
            lo, hi = self._sel
            self.set_selection((min(lo, line), max(hi, line)))
        elif self._sel == (line, line):
            self.set_selection(None)                  # click again to clear
        else:
            self.set_selection((line, line))

    def set_selection(self, sel):
        app = self.app
        self._sel = sel
        self.canvas.delete("sel")
        if not sel:
            return
        lo, hi = sel
        y0 = app.pad + (lo - 1) * app.line_h
        y1 = app.pad + hi * app.line_h
        self.canvas.create_rectangle(0, y0, self.content_width, y1,
                                     fill=blend(BG, (130, 160, 190), 0.16),
                                     outline="", tags="sel")
        if self.canvas.find_withtag("code"):
            self.canvas.tag_lower("sel", "code")
        n = hi - lo + 1
        app.status_hint.config(
            text="%d line%s selected  •  Ctrl+C to copy" %
                 (n, "s" if n != 1 else ""))

    def copy_selection(self, _event=None):
        app = self.app
        widget = app.root.focus_get()
        if isinstance(widget, (tk.Entry, tk.Text)):
            return                       # let the field's own copy work
        if not (app.file_path and self._sel):
            return
        lo, hi = self._sel
        app.root.clipboard_clear()
        app.root.clipboard_append("\n".join(app.lines[lo - 1:hi]))
        n = hi - lo + 1
        app._flash_status("Copied %d line%s to clipboard" %
                          (n, "s" if n != 1 else ""))

    # -------------------------------------------------- annotation gestures

    def _gutter_line(self, event):
        app = self.app
        y = self.gutter.canvasy(event.y) - app.pad
        return max(1, min(len(app.lines), int(y // app.line_h) + 1))

    def _gutter_press(self, event):
        app = self.app
        if not app.file_path:
            return
        if app._paste_ann:
            app._paste_comment_at(self._gutter_line(event))
            return
        if app._ann_error:
            app._flash_status("Annotating is disabled — this file's "
                              "annotation file could not be parsed")
            return
        self._drag_start = self._gutter_line(event)
        self._pending = (self._drag_start, self._drag_start)
        self.draw_annotations()

    def _gutter_drag(self, event):
        if self._drag_start is None:
            return
        line = self._gutter_line(event)
        self._pending = (min(self._drag_start, line),
                         max(self._drag_start, line))
        self.draw_annotations()

    def _gutter_release(self, event):
        app = self.app
        if self._drag_start is None or not self._pending:
            return
        lo, hi = self._pending
        self._drag_start = None
        ann = {"id": uuid.uuid4().hex[:8], "startLine": lo, "endLine": hi,
               "color": "yellow", "text": "",
               "anchor": anchor_text(app.lines[lo - 1])}
        app._edit_annotation(ann, is_new=True,
                             x=event.x_root, y=event.y_root)

    def _on_ann_click(self, event):
        app = self.app
        if self._dragged:
            return                       # end of a drag-selection, not a click
        items = self.canvas.find_withtag("current")
        if not items:
            return
        ann_id = next((t.split("_", 1)[1] for t in self.canvas.gettags(items[0])
                       if t.startswith("ann_")), None)
        ann = next((a for a in app.annotations if a["id"] == ann_id), None)
        if ann:
            app._edit_annotation(ann, is_new=False,
                                 x=event.x_root, y=event.y_root)


# ------------------------------------------------------------------ the app

class CodeAnnotatorApp:
    def __init__(self, root, folder=None, ann_root=None, settings_path=None):
        self.root = root
        root.title("Code Annotator")
        root.configure(bg=BG)

        self.settings_path = settings_path
        settings = load_settings(settings_path) if settings_path else {}
        geometry = str(settings.get("geometry", "")) or "1400x900"
        try:
            root.geometry(geometry)
        except tk.TclError:
            root.geometry("1400x900")

        global UI_FONT
        available = set(tkfont.families(root))
        UI_FONT = pick_font(UI_FONTS, available)
        size = settings.get("font_size", 11)
        size = size if isinstance(size, int) and 6 <= size <= 28 else 11
        self.code_font = tkfont.Font(family=pick_font(CODE_FONTS, available),
                                     size=size)
        self.note_font = tkfont.Font(family=UI_FONT, size=max(7, size - 2),
                                     slant="italic")
        self.char_w = self.code_font.measure("0")
        self.line_h = self.code_font.metrics("linespace") + 4
        self.pad = 8

        self.workspace = Workspace(ann_root)
        self.file_path = None          # currently open file
        self.lines = []
        self.annotations = []
        self.show_annotations = tk.BooleanVar(
            value=bool(settings.get("show_annotations", True)))
        self.opacity = tk.DoubleVar(value=float(settings.get("opacity", 55)))
        saved_colors = set(settings.get("visible_colors",
                                        list(ANN_COLORS))) & set(ANN_COLORS)
        self.visible_colors = saved_colors or set(ANN_COLORS)
        self._ann_idx = None           # annotation the user last jumped to
        self._paste_ann = None         # annotation being placed as a comment
        self._flash_msg = None         # transient status-bar message
        self._flash_after = None       # pending flash-clear timer id
        self._ann_error = False        # sidecar unparseable; saving disabled
        self.lang_id = "plain"
        self._truncated = False        # current file cut off at MAX_LINES
        self._deleted_ann = None       # (path, ann) one-slot delete undo
        self._sidecar_mtime = None     # for detecting external ann changes

        self._build_ui()
        root.protocol("WM_DELETE_WINDOW", self._on_close)
        root.after(SIDECAR_POLL_MS, self._poll_sidecar)
        if not folder:
            last = settings.get("folder")
            if last and os.path.isdir(last):
                folder = last
        if folder:
            self.open_folder(folder)

    # ---------------------------------------------------------------- UI

    def _build_ui(self):
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure("Treeview", background=BG_PANEL, foreground=FG,
                        fieldbackground=BG_PANEL, borderwidth=0,
                        font=(UI_FONT, 10), rowheight=22)
        style.map("Treeview", background=[("selected", BG_ACTIVE)],
                  foreground=[("selected", "#ffffff")])
        style.configure("Vertical.TScrollbar", background=BG_ACTIVE,
                        troughcolor=BG_PANEL, borderwidth=0, arrowcolor=FG_DIM)
        style.configure("Horizontal.TScrollbar", background=BG_ACTIVE,
                        troughcolor=BG_PANEL, borderwidth=0, arrowcolor=FG_DIM)

        # --- toolbar ---
        bar = tk.Frame(self.root, bg=BG_PANEL)
        bar.pack(fill="x")
        tk.Button(bar, text="\U0001F4C2 Open Folder", command=self.pick_folder,
                  bg=ACCENT, fg="#ffffff", relief="flat", cursor="hand2",
                  font=(UI_FONT, 10), padx=10).pack(side="left", padx=8, pady=5)
        self.edit_btn = tk.Button(bar, text="✎ Edit",
                                  command=lambda: self.edit.toggle(),
                                  bg=BG_ACTIVE,
                                  fg=FG, relief="flat", cursor="hand2",
                                  font=(UI_FONT, 10), padx=10)
        self.edit_btn.pack(side="left", padx=(0, 4), pady=5)
        self.save_btn = tk.Button(bar, text="\U0001F4BE Save",
                                  command=lambda: self.edit.save(),
                                  bg=BG_ACTIVE,
                                  fg=FG, relief="flat", cursor="hand2",
                                  font=(UI_FONT, 10), padx=10)
        # save_btn is packed only while editing
        self.folder_label = tk.Label(bar, text="No folder open", bg=BG_PANEL,
                                     fg=FG_DIM, font=(UI_FONT, 10))
        self.folder_label.pack(side="left", padx=4)

        tk.Scale(bar, from_=10, to=100, orient="horizontal", length=110,
                 variable=self.opacity, showvalue=False, bg=BG_PANEL,
                 troughcolor=BG_ACTIVE, highlightthickness=0, sliderrelief="flat",
                 command=self._on_opacity).pack(side="right", padx=(2, 12))
        tk.Label(bar, text="Opacity", bg=BG_PANEL, fg=FG_DIM,
                 font=(UI_FONT, 10)).pack(side="right")
        chip_row = tk.Frame(bar, bg=BG_PANEL)   # per-color visibility filter
        chip_row.pack(side="right", padx=(0, 6))
        self._chips = {}
        for name in ANN_COLORS:
            chip = tk.Canvas(chip_row, width=16, height=16, bg=BG_PANEL,
                             highlightthickness=0, cursor="hand2")
            chip.pack(side="left", padx=1)
            chip.bind("<Button-1>", lambda e, nm=name: self._toggle_color(nm))
            self._chips[name] = chip
        self._draw_chips()
        tk.Checkbutton(bar, text="Annotations", variable=self.show_annotations,
                       command=lambda: self.viewer.draw_annotations(),
                       bg=BG_PANEL, fg=FG_DIM,
                       selectcolor=BG, activebackground=BG_PANEL,
                       activeforeground=FG, font=(UI_FONT, 10)).pack(
            side="right", padx=10)

        # --- main split: tree | code view ---
        paned = tk.PanedWindow(self.root, orient="horizontal", bg="#3c3c3c",
                               sashwidth=4, borderwidth=0)
        paned.pack(fill="both", expand=True)

        self.tree_pane = FileTreePane(self, paned)

        view = tk.Frame(paned, bg=BG)
        paned.add(view, minsize=400)

        self.viewer = ViewerPane(self, view)
        self.finder = FindController(self, self.viewer.code_frame)
        self.edit = EditorPane(self, view)
        self._bind_shortcuts()

        # --- status bar ---
        status = tk.Frame(self.root, bg="#007acc")
        status.pack(fill="x")
        self.status_file = tk.Label(status, text="Ready", bg="#007acc",
                                    fg="#ffffff", font=(UI_FONT, 9))
        self.status_file.pack(side="left", padx=10)
        self.status_hint = tk.Label(
            status, text="Click or drag on line numbers to annotate • "
                         "click a highlight to edit • Ctrl+F to search",
            bg="#007acc", fg="#cde6f7", font=(UI_FONT, 9))
        self.status_hint.pack(side="right", padx=10)

        self.viewer.show_welcome()

    def _bind_shortcuts(self):
        binds = (("<Control-f>", self._open_find),
                 ("<Control-g>", self.finder.open_goto),
                 ("<F3>", self._find_next),
                 ("<Shift-F3>", self._find_prev),
                 ("<Escape>", self._close_bars),
                 ("<Control-z>", self._undo_delete),
                 ("<Control-c>", lambda e: self.viewer.copy_selection(e)),
                 ("<Control-plus>", lambda e: self._zoom(1)),
                 ("<Control-equal>", lambda e: self._zoom(1)),
                 ("<Control-minus>", lambda e: self._zoom(-1)),
                 ("<Control-Key-0>", lambda e: self._zoom(0)),
                 ("<Alt-Up>", lambda e: self._goto_annotation(-1)),
                 ("<Alt-Down>", lambda e: self._goto_annotation(1)),
                 ("<Return>", self._paste_default),
                 ("<Control-s>", self.edit.save))
        for seq, fn in binds:
            self.root.bind(seq, fn)

    def _open_find(self, _event=None):
        """Ctrl+F: in-editor search while editing, canvas search otherwise."""
        if self.edit.active:
            return self.edit.open_find()
        return self.finder.open_find()

    def _find_next(self, _event=None):
        return (self.edit.find_next() if self.edit.active
                else self.finder.next())

    def _find_prev(self, _event=None):
        return (self.edit.find_prev() if self.edit.active
                else self.finder.prev())

    def _draw_chips(self):
        for name, chip in self._chips.items():
            chip.delete("all")
            rgb = ANN_COLORS[name]
            if name in self.visible_colors:
                chip.create_oval(2, 2, 14, 14, outline="",
                                 fill="#%02x%02x%02x" % rgb)
            else:
                chip.create_oval(4, 4, 12, 12, width=2,
                                 outline=blend(BG_PANEL, rgb, 0.45))

    def _toggle_color(self, name):
        if name in self.visible_colors:
            self.visible_colors.discard(name)
        else:
            self.visible_colors.add(name)
        self._draw_chips()
        self.viewer.draw_annotations()
        if self.edit.active:
            self.edit.update_band_colors()

    # ------------------------------------------------------------ folder tree

    def pick_folder(self):
        folder = filedialog.askdirectory(title="Open project folder")
        if folder:
            self.open_folder(folder)

    def open_folder(self, folder):
        if self.edit.active and not self.edit.exit():
            return
        self.workspace.open(folder)
        self.folder_label.config(
            text=self.workspace.folder + "   •   annotations: "
                 + self.workspace.ann_root)
        self.tree_pane.show_folder(self.workspace.folder)

    # ------------------------------------------------------------- file view

    def open_file(self, path):
        try:
            size = os.path.getsize(path)
            if size > MAX_FILE_BYTES:
                self._show_message("%s is too large to display (%.1f MB)."
                                   % (os.path.basename(path), size / 1048576.0))
                return
            with open(path, "rb") as f:
                head = f.read(8192)
            if b"\x00" in head:
                self._show_message(os.path.basename(path)
                                   + " looks like a binary file.")
                return
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError as exc:
            self._show_message("Could not open file: %s" % exc)
            return

        lines = [ln.expandtabs(TAB_WIDTH) for ln in text.splitlines()] or [""]
        truncated = len(lines) > MAX_LINES
        if truncated:
            lines = lines[:MAX_LINES]
        self._truncated = truncated

        self.file_path = path
        self.lines = lines
        sidecar = self.workspace.sidecar_for(path)
        repaired = lost = 0
        try:
            self.annotations = load_annotations(sidecar)
            self._ann_error = False
            if not truncated:
                repaired, lost = reconcile_annotations(self.annotations,
                                                       lines)
                if repaired or lost:
                    self._persist()      # keep the healed positions
        except AnnotationLoadError:
            self.annotations = []
            self._ann_error = True
        try:
            self._sidecar_mtime = os.path.getmtime(sidecar)
        except OSError:
            self._sidecar_mtime = None
        self._ann_idx = None
        self._paste_ann = None

        lang_id = lang_for(os.path.basename(path))
        self.lang_id = lang_id
        self.viewer.render(highlight_lines(lines, lang_id))
        self.viewer.draw_annotations()
        self.finder.on_file_opened()
        self.tree_pane.refresh_outline(self.annotations)

        name = os.path.basename(path)
        lang_note = "" if lang_id == "plain" else "  •  " + lang_id.upper()
        trunc_note = ("  •  showing first %d lines" % MAX_LINES) if truncated else ""
        self.status_file.config(text=name + lang_note + trunc_note)
        if self._ann_error:
            self._flash_status("Annotation file is unreadable — annotating "
                               "is disabled for this file until it is fixed "
                               "or deleted", ms=8000)
        elif repaired or lost:
            parts = []
            if repaired:
                parts.append(f"{repaired} annotation(s) re-anchored to "
                             "moved code")
            if lost:
                parts.append(f"{lost} annotation(s) orphaned — their "
                             "anchored code is gone")
            self._flash_status(" • ".join(parts), ms=6000)

    def _show_message(self, msg):
        self.file_path = None
        self.lines = []
        self.annotations = []
        self.finder.reset()
        self._ann_idx = None
        self._paste_ann = None
        self._ann_error = False
        self._sidecar_mtime = None
        self.tree_pane.refresh_outline([])
        self.viewer.clear(msg)
        self.status_file.config(text="Ready")

    def _on_opacity(self, _value=None):
        self.viewer.draw_annotations()
        if self.edit.active:
            self.edit.update_band_colors()

    def _close_bars(self, _event=None):
        """Escape: dismiss find/goto bars, selection, and paste placement."""
        if self.edit.active:
            return
        if not (self.finder.visible() or self.viewer._sel
                or self.finder.matches or self._paste_ann):
            return
        self._paste_ann = None
        self.finder.close_bars()
        self.viewer.set_selection(None)
        self.viewer.draw_annotations()   # restores the default status hint
        self.viewer.canvas.focus_set()

    def _goto_annotation(self, step, _event=None):
        """Alt+Down / Alt+Up: cycle to the next / previous annotation."""
        if self.edit.active:
            return
        anns = self.annotations           # kept sorted by startLine
        if not (self.file_path and anns):
            return "break"
        idx = self._ann_idx
        if (idx is not None and 0 <= idx < len(anns)
                and self.viewer.line_visible(anns[idx]["startLine"])):
            idx = (idx + step) % len(anns)
        else:
            # lost track (scrolled away, file changed): pick relative to
            # the view — next scans down from its top edge, previous scans
            # up from its bottom edge, so visible annotations come first
            top, bottom = self.viewer.viewport_lines()
            if step > 0:
                idx = next((i for i, a in enumerate(anns)
                            if a["startLine"] > top), 0)
            else:
                idx = next((i for i in range(len(anns) - 1, -1, -1)
                            if anns[i]["startLine"] < bottom),
                           len(anns) - 1)
        self.show_annotation(idx)
        return "break"

    def show_annotation(self, idx):
        """Jump to annotation ``idx``: scroll, flash, and announce it."""
        anns = self.annotations
        if self.edit.active or not (self.file_path and anns):
            return
        idx = max(0, min(idx, len(anns) - 1))
        self._ann_idx = idx
        a = anns[idx]
        self.viewer.scroll_to_line(a["startLine"])
        self.viewer.flash_annotation(a)
        note = a["text"].replace("\n", " ")
        if len(note) > 60:
            note = note[:57] + "..."
        self.status_hint.config(
            text="Annotation %d of %d%s" %
                 (idx + 1, len(anns), "  •  " + note if note else ""))

    def _zoom(self, delta, _event=None):
        cur = self.code_font.cget("size")
        size = 11 if delta == 0 else max(6, min(28, cur + delta))
        if size != cur:
            self.code_font.configure(size=size)
            self.note_font.configure(size=max(7, size - 2))
            self.char_w = self.code_font.measure("0")
            self.line_h = self.code_font.metrics("linespace") + 4
            if self.edit.active:
                self.edit.on_zoom()
            else:
                self.viewer.on_zoom()
        return "break"

    # -------------------------------------------------- line selection / copy

    def _flash_status(self, text, ms=4000):
        """Show a status-bar message that survives redraws for a few seconds."""
        self._flash_msg = text
        self.status_hint.config(text=text)
        if self._flash_after is not None:   # an older timer would clear the
            self.root.after_cancel(self._flash_after)   # new message early
        self._flash_after = self.root.after(ms, self._clear_flash)

    def _clear_flash(self):
        self._flash_after = None
        self._flash_msg = None
        if self.edit.active:
            self.edit._set_hint()
        else:
            self.viewer.draw_annotations()      # restores the regular hint

    def _save_settings(self):
        if not self.settings_path:
            return
        data = {"geometry": self.root.geometry(),
                "font_size": self.code_font.cget("size"),
                "opacity": self.opacity.get(),
                "show_annotations": bool(self.show_annotations.get()),
                "visible_colors": sorted(self.visible_colors),
                "folder": self.workspace.folder}
        try:
            with open(self.settings_path, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)
        except OSError:
            pass                           # settings are best-effort

    def _on_close(self):
        if self.edit.active and not self.edit.exit():
            return                         # user cancelled the close
        self._save_settings()
        self.root.destroy()

    # -------------------------------------------------- annotation interaction

    def _edit_annotation(self, ann, is_new, x, y):
        can_paste = (comment_lines("x", self.lang_id) is not None
                     and not self._truncated)
        dialog = AnnotationDialog(self.root, ann, is_new, x, y,
                                  can_paste=can_paste)
        self.root.wait_window(dialog)
        self.viewer.canvas.focus_set()          # keyboard focus back to the app
        self.viewer._pending = None             # clear the gutter drag band
        if dialog.result in ("save", "paste"):
            ann["orphaned"] = False      # user re-confirmed this location
            if 1 <= ann["startLine"] <= len(self.lines):
                ann["anchor"] = anchor_text(self.lines[ann["startLine"] - 1])
            if is_new:
                self.annotations.append(ann)
                self.annotations.sort(key=lambda a: a["startLine"])
            self._ann_idx = None         # indices shifted; re-anchor on jump
            self._persist()
        elif dialog.result == "delete":
            self.annotations = [a for a in self.annotations
                                if a["id"] != ann["id"]]
            self._ann_idx = None
            self._deleted_ann = (self.file_path, dict(ann))
            self._persist()
            self._flash_status("Annotation deleted — Ctrl+Z restores",
                               ms=6000)
        self.viewer.draw_annotations()
        if dialog.result == "paste" and ann["text"]:
            self._start_paste(ann)

    # --------------------------------------------- paste note as code comment

    def _start_paste(self, ann):
        """Arm placement mode: the next line click inserts the comment."""
        self._paste_ann = ann
        self.status_hint.config(
            text="Insert note as comment (opens the editor): click a line "
                 "number to place it • Enter = line %d (region start) • "
                 "Esc cancels" % ann["startLine"])

    def _paste_default(self, _event=None):
        if self._paste_ann:
            self._paste_comment_at(self._paste_ann["startLine"])
            return "break"

    def _paste_comment_at(self, line_no):
        """Insert the armed annotation's note as comment lines above
        ``line_no`` — as a pending, undoable edit in the editor. Nothing is
        written to disk until the user saves; annotation anchors follow the
        inserted lines through their editor band tags."""
        ann, self._paste_ann = self._paste_ann, None
        rendered = comment_lines(ann["text"], self.lang_id)
        if not rendered:
            self.viewer.draw_annotations()
            return
        if not self.edit.active:
            self.edit.enter()
            if not self.edit.active:      # refused (non-UTF8, unreadable, …)
                return
        total = int(self.edit.editor.index("end-1c").split(".")[0])
        line_no = max(1, min(line_no, total))
        target = self.edit.editor.get("%d.0" % line_no, "%d.end" % line_no)
        indent = target[:len(target) - len(target.lstrip(" \t"))]
        block = "".join(indent + ln + "\n" for ln in rendered)
        self.edit.editor.edit_separator()     # one Ctrl+Z removes the whole block
        self.edit.editor.insert("%d.0" % line_no, block)
        self.edit.editor.edit_separator()
        self.edit.editor.mark_set("insert", "%d.0" % line_no)
        self.edit.editor.see("%d.0" % line_no)
        self.edit.editor.focus_set()
        self._flash_status(
            "Inserted %d comment line%s at line %d — Ctrl+Z undoes, "
            "Ctrl+S saves" % (len(rendered),
                              "s" if len(rendered) != 1 else "", line_no))

    def _undo_delete(self, _event=None):
        """Ctrl+Z in the annotate view: restore the last deleted annotation."""
        if self.edit.active:
            return                       # the editor has its own undo stack
        widget = self.root.focus_get()
        if isinstance(widget, (tk.Entry, tk.Text)):
            return
        if not self._deleted_ann:
            return
        path, ann = self._deleted_ann
        if path != self.file_path or self._ann_error:
            return
        self._deleted_ann = None
        self.annotations.append(ann)
        self.annotations.sort(key=lambda a: a["startLine"])
        self._persist()
        self.viewer.draw_annotations()
        self._flash_status("Annotation restored")
        return "break"

    def _poll_sidecar(self):
        """Live-reload annotations when the sidecar changes outside the app
        (e.g. an LLM writing annotations while a file is open)."""
        self.root.after(SIDECAR_POLL_MS, self._poll_sidecar)
        if not self.file_path or self.edit.active or self._ann_error:
            return
        sidecar = self.workspace.sidecar_for(self.file_path)
        try:
            mtime = os.path.getmtime(sidecar)
        except OSError:
            mtime = None
        if mtime == self._sidecar_mtime:
            return
        self._sidecar_mtime = mtime
        try:
            self.annotations = load_annotations(sidecar)
        except AnnotationLoadError:
            self._ann_error = True
            self.annotations = []
            self._flash_status("Annotation file became unreadable — "
                               "annotating disabled", ms=8000)
            return
        # heal in memory only: don't write back while another tool is writing
        reconcile_annotations(self.annotations, self.lines)
        self._ann_idx = None
        self.viewer.draw_annotations()
        self.tree_pane.refresh_outline(self.annotations)
        self.tree_pane.refresh_badge(self.file_path)
        self._flash_status("Annotations reloaded (changed outside the app)")

    def _persist(self):
        if self._ann_error:
            from tkinter import messagebox
            messagebox.showerror(
                "Annotations locked",
                "This file's annotation file exists but could not be "
                "parsed:\n%s\n\nSaving is disabled so its contents are not "
                "overwritten. Fix or delete it, then reopen the file."
                % self.workspace.sidecar_for(self.file_path))
            return
        try:
            self.workspace.save(self.file_path, self.annotations)
            self.tree_pane.refresh_badge(self.file_path)
            self.tree_pane.refresh_outline(self.annotations)
            try:
                self._sidecar_mtime = os.path.getmtime(
                    self.workspace.sidecar_for(self.file_path))
            except OSError:
                self._sidecar_mtime = None
        except OSError as exc:
            from tkinter import messagebox
            messagebox.showerror("Save failed",
                                 "Could not write annotation file:\n%s" % exc)


def main():
    parser = argparse.ArgumentParser(
        description="Browse a project folder and view code files with "
                    "annotations overlaid transparently. Annotations are "
                    "kept in a parallel tree mirroring the source layout.")
    parser.add_argument("folder", nargs="?",
                        help="project folder to open at startup")
    parser.add_argument("-a", "--annotations", metavar="ANN_ROOT",
                        help="root of the annotation tree (default: "
                             + ANN_DIR_NAME + "/ under the git repo root "
                             "containing the folder, or under the folder "
                             "itself when not in a git repo)")
    parser.add_argument("--check", action="store_true",
                        help="validate all annotation files against their "
                             "sources and exit (no GUI)")
    parser.add_argument("--fix", action="store_true",
                        help="with --check: re-anchor drifted annotations "
                             "and rewrite the repaired files")
    args = parser.parse_args()

    if args.check:
        sys.exit(check_annotations(args.folder or ".", fix=args.fix,
                                   ann_root=args.annotations))

    root = tk.Tk()
    settings_path = os.path.join(os.path.expanduser("~"),
                                 ".code_annotator.json")
    CodeAnnotatorApp(root, args.folder, ann_root=args.annotations,
                     settings_path=settings_path)
    root.mainloop()


if __name__ == "__main__":
    main()
