# -*- coding: utf-8 -*-
"""Core non-GUI logic: tokenizer, color blending, annotation persistence."""
import os
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import code_annotator as ca

# --- color blending ---
assert ca.blend("#000000", (255, 255, 255), 0.5) == "#808080"
assert ca.blend("#1e1e1e", (230, 190, 50), 0.0) == "#1e1e1e"
assert ca.blend("#1e1e1e", (230, 190, 50), 1.0) == "#e6be32"
print("blend OK")

# --- tokenizer ---
toks = ca.highlight_lines(['def foo(x):  # hi', '    return "s" + 1'], "py")
flat = [(t[1], t[2]) for line in toks for t in line]
assert ("def", "kw") in flat and ("# hi", "com") in flat
assert ('"s"', "str") in flat and ("1", "num") in flat

toks = ca.highlight_lines(['"""doc', 'still string', 'end"""', "x = 1"], "py")
assert toks[1][0][2] == "str", "triple-quote state not carried across lines"
assert ("x", None) in [(t[1], t[2]) for t in toks[3]]

toks = ca.highlight_lines(["<div class='a'><!-- c -->"], "html")
flat = [(t[1], t[2]) for line in toks for t in line]
assert ("div", "tag") in flat and ("class", "attr") in flat

assert ca.lang_for("Makefile") == "sh"
assert ca.lang_for("x.rs") == "rust"
assert ca.lang_for("noext") == "plain"
print("tokenizer OK")

# --- comment rendering ---
assert ca.comment_lines("hi\nthere", "py") == ["# hi", "# there"]
assert ca.comment_lines("hi", "json") is None
print("comment_lines OK")

# --- annotation persistence ---
tmp = tempfile.mkdtemp()
try:
    ann_root = os.path.join(tmp, ".annotations")
    ann_path = os.path.join(ann_root, "src", "foo.py.ann.json")
    anns = [{"id": "abcd1234", "startLine": 3, "endLine": 5,
             "color": "green", "text": "note", "orphaned": False,
             "anchor": "x = compute()"}]
    ca.save_annotations(ann_path, "src/foo.py", anns, prune_root=ann_root)
    loaded = ca.load_annotations(ann_path)
    assert loaded == anns, loaded
    raw = open(ann_path, encoding="utf-8").read()
    assert "orphaned" not in raw, "falsy orphaned flag should not be written"
    assert "x = compute()" in raw, "anchor should be persisted"

    # reconcile: drifted anchor is re-located, missing anchor orphans
    lines = ["pad", "pad", "pad", "x = compute()", "y = 2"]
    moved = [dict(anns[0])]
    repaired, lost = ca.reconcile_annotations(moved, lines)
    assert (repaired, lost) == (1, 0)
    assert (moved[0]["startLine"], moved[0]["endLine"]) == (4, 5)
    gone = [dict(anns[0])]
    repaired, lost = ca.reconcile_annotations(gone, ["a", "b"])
    assert (repaired, lost) == (0, 1) and gone[0]["orphaned"] is True
    assert ca.anchor_text("  x =\t compute()  ") == "x = compute()"
    assert not [f for f in os.listdir(os.path.dirname(ann_path))
                if f.endswith(".tmp~")], "temp file left behind"

    # missing file -> [], corrupt file -> raises (never silently empty)
    assert ca.load_annotations(os.path.join(tmp, "nope.ann.json")) == []
    bad = os.path.join(tmp, "bad.ann.json")
    open(bad, "w").write("{ not json !!")
    try:
        ca.load_annotations(bad)
        raise AssertionError("corrupt file did not raise")
    except ca.AnnotationLoadError:
        pass

    # removing the last annotation deletes the file and prunes empty dirs
    ca.save_annotations(ann_path, "src/foo.py", [], prune_root=ann_root)
    assert not os.path.exists(ann_path)
    assert not os.path.exists(os.path.dirname(ann_path))
    print("persistence OK")
finally:
    shutil.rmtree(tmp)

print("ALL CORE CHECKS PASSED")
