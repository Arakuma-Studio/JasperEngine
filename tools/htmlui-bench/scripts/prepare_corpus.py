#!/usr/bin/env python3
"""Builds the benchmark sets from the fetched sources (called by fetch_corpus.sh).

    prepare_corpus.py CORPUS_DIR

Writes CORPUS_DIR/sets/<set>/*.svg (currentColor -> #000000, so nanosvg, which has no
currentColor, draws the same thing as the others and the references) and CORPUS_DIR/<set>.txt
(paths relative to sets/<set>). The conformance set is the resvg test suite used unmodified
in place: conformance.txt lists paths relative to src/resvg/crates/resvg/tests/tests.
Only the standard library is used.
"""
import os
import random
import re
import shutil
import sys

FLUENT_SEED = 20261007
FLUENT_COUNT = 300
HUGE_MIN_BYTES = 300 * 1024


def write_set(corpus, name, files):
    """files: list of (output name, source path). Returns the number of files written."""
    out = os.path.join(corpus, "sets", name)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    names = []
    for out_name, src in files:
        with open(src, "rb") as f:
            data = f.read()
        data = data.replace(b"currentColor", b"#000000")
        with open(os.path.join(out, out_name), "wb") as f:
            f.write(data)
        names.append(out_name)
    with open(os.path.join(corpus, name + ".txt"), "w") as f:
        for n in sorted(names):
            f.write(n + "\n")
    return len(names)


def main():
    corpus = os.path.abspath(sys.argv[1])
    src = os.path.join(corpus, "src")

    lucide = os.path.join(src, "lucide", "icons")
    icons = sorted(f for f in os.listdir(lucide) if f.endswith(".svg"))
    n = write_set(corpus, "icons", [(f, os.path.join(lucide, f)) for f in icons])
    print(f"  icons: {n}")

    assets = os.path.join(src, "fluentui-emoji", "assets")
    fluent = []
    for emoji in sorted(os.listdir(assets)):
        color = os.path.join(assets, emoji, "Color")
        if os.path.isdir(color):
            fluent += [os.path.join(color, f) for f in sorted(os.listdir(color)) if f.endswith(".svg")]
    chosen = sorted(random.Random(FLUENT_SEED).sample(fluent, FLUENT_COUNT))
    n = write_set(corpus, "multicolor", [(os.path.basename(p), p) for p in chosen])
    print(f"  multicolor: {n} of {len(fluent)} (seed {FLUENT_SEED})")

    example = os.path.join(src, "thorvg.example", "res", "svg")
    ill = sorted(f for f in os.listdir(example) if f.endswith(".svg"))
    n = write_set(corpus, "illustrations", [(f, os.path.join(example, f)) for f in ill])
    print(f"  illustrations: {n}")

    huge = [("BlankMap-World.svg", os.path.join(src, "wikimedia", "BlankMap-World.svg"))]
    huge += [(f, os.path.join(example, f)) for f in ill if os.path.getsize(os.path.join(example, f)) > HUGE_MIN_BYTES]
    n = write_set(corpus, "huge", huge)
    print(f"  huge: {n} ({', '.join(h[0] for h in huge)})")

    # resvg suite: the tests its harness renders in normal mode (width scaled to 300 px).
    tests = os.path.join(src, "resvg", "crates", "resvg", "tests")
    with open(os.path.join(tests, "integration", "render.rs")) as f:
        names = re.findall(r'render\("tests/([^"]+)"\)', f.read())
    names = [n for n in names if os.path.exists(os.path.join(tests, "tests", n + ".svg"))
             and os.path.exists(os.path.join(tests, "tests", n + ".png"))]
    with open(os.path.join(corpus, "conformance.txt"), "w") as f:
        for name in sorted(names):
            f.write(name + ".svg\n")
    print(f"  conformance: {len(names)} (root {os.path.join(tests, 'tests')})")


if __name__ == "__main__":
    main()
