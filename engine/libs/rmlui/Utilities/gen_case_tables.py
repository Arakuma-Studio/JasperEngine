#!/usr/bin/env python3
"""Generates the Unicode tables of the fork's text code (phase-3 spec 3.10, p3-text).

    Utilities/gen_case_tables.py [--ucd DIR] [--version 16.0.0]

writes, next to this script's Source/Core:

    UnicodeCaseTables.inl       text-transform: the simple case mappings of UnicodeData.txt
                                (upper, lower, and title where it is not upper; BMP and
                                supplementary planes) and the unconditional full upper and lower
                                mappings of SpecialCasing.txt (German sharp s -> "SS",
                                ligatures, ...) for 'uppercase' and 'lowercase'; 'capitalize'
                                takes the simple titlecase mapping, as Chromium does; the
                                letters and digits that make up words (general categories L, M,
                                N)
    UnicodeLineBreakTables.inl  line-break opportunities: the UAX #14 classes the fork's line
                                breaker tells apart (LineBreak.txt), every other code point is AL

The Unicode Character Database files (UnicodeData.txt, SpecialCasing.txt, LineBreak.txt) are
read from --ucd, or downloaded from unicode.org into build/unicode-<version>/ of the repository.
The output is deterministic: run it again after a Unicode update and commit both files.
"""

import argparse
import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "Source", "Core")
FILES = ("UnicodeData.txt", "SpecialCasing.txt", "LineBreak.txt")

# The line-break classes the line breaker (TextUnicode.cpp) distinguishes, in the order of the
# LineBreakClass enum; every other class (AL, SA, AI, XX, PR, PO, SY, ...) reads as AL there.
# SP, BK, CR, LF, NL are white space or segment breaks, handled before the classes are asked.
LINE_BREAK_CLASSES = ["AL", "ZW", "GL", "BA", "HY", "B2", "ID", "CL", "CP", "EX", "IS", "NS", "OP", "QU", "CM", "ZWJ",
                      "NU", "IN", "EB", "EM", "BB", "H2", "H3", "JL", "JV", "JT"]
# read as one of the classes above (CSS 'line-break: normal': small kana are ideographs)
LINE_BREAK_ALIASES = {"CJ": "ID", "WJ": "GL", "HL": "AL", "AI": "AL", "SA": "AL", "XX": "AL", "PR": "AL", "PO": "AL", "SY": "AL",
                      "CB": "AL", "SG": "AL", "AK": "AL", "AP": "AL", "AS": "AL", "VF": "AL", "VI": "AL", "RI": "AL",
                      "SP": "AL", "BK": "AL", "CR": "AL", "LF": "AL", "NL": "AL"}


def ucd_paths(args):
    if args.ucd:
        return {name: os.path.join(args.ucd, name) for name in FILES}
    cache = os.path.join(HERE, "..", "..", "..", "..", "build", "unicode-" + args.version)
    os.makedirs(cache, exist_ok=True)
    paths = {}
    for name in FILES:
        path = os.path.join(cache, name)
        if not os.path.exists(path):
            url = "https://www.unicode.org/Public/%s/ucd/%s" % (args.version, name)
            print("gen_case_tables: downloading " + url)
            urllib.request.urlretrieve(url, path)
        paths[name] = path
    return paths


def read_lines(path):
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                yield line


def unicode_data(path):
    """code -> (category, upper, lower, title), ranges expanded"""
    entries = {}
    first = None
    for line in read_lines(path):
        fields = line.split(";")
        code = int(fields[0], 16)
        name = fields[1]
        category = fields[2]
        upper = int(fields[12], 16) if fields[12] else None
        lower = int(fields[13], 16) if fields[13] else None
        title = int(fields[14], 16) if len(fields) > 14 and fields[14] else None
        if name.endswith(", First>"):
            first = code
            continue
        if name.endswith(", Last>"):
            for c in range(first, code + 1):
                entries[c] = (category, None, None, None)
            continue
        entries[code] = (category, upper, lower, title)
    return entries


def special_casing(path):
    """unconditional full mappings: code -> (lower, title, upper) as tuples of code points"""
    entries = {}
    for line in read_lines(path):
        fields = [f.strip() for f in line.split(";")]
        if len(fields) > 4 and fields[4]:
            continue   # conditional (language or context): not applied, as CSS without a lang tailoring
        code = int(fields[0], 16)
        parse = lambda f: tuple(int(x, 16) for x in f.split())
        entries[code] = (parse(fields[1]), parse(fields[2]), parse(fields[3]))
    return entries


def ranges_of(codes):
    """sorted code points -> [(first, last)]"""
    out = []
    for c in sorted(codes):
        if out and out[-1][1] + 1 == c:
            out[-1][1] = c
        else:
            out.append([c, c])
    return out


def line_break_classes(path):
    classes = {}
    for line in read_lines(path):
        code, cls = [f.strip() for f in line.split(";")]
        cls = LINE_BREAK_ALIASES.get(cls, cls)
        if cls not in LINE_BREAK_CLASSES:
            raise SystemExit("gen_case_tables: unknown line-break class " + cls)
        if ".." in code:
            a, b = code.split("..")
            a, b = int(a, 16), int(b, 16)
        else:
            a = b = int(code, 16)
        for c in range(a, b + 1):
            classes[c] = cls
    # unlisted code points: the UAX #14 defaults for unassigned ideographic ranges are ID
    for a, b in ((0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xF900, 0xFAFF), (0x20000, 0x2FFFD), (0x30000, 0x3FFFD), (0x1F000, 0x1FAFF),
                 (0x1FC00, 0x1FFFD)):
        for c in range(a, b + 1):
            classes.setdefault(c, "ID")
    runs = []
    for c in sorted(classes):
        cls = classes[c]
        if runs and runs[-1][1] + 1 == c and runs[-1][2] == cls:
            runs[-1][1] = c
        else:
            runs.append([c, c, cls])
    # the default needs no entry
    return [r for r in runs if r[2] != "AL"]


def hex_list(values, per_line, width):
    lines = []
    for i in range(0, len(values), per_line):
        lines.append("\t" + " ".join(("0x%0" + str(width) + "X,") % v for v in values[i:i + per_line]))
    return "\n".join(lines)


HEADER = """/*
 * This file is generated by Utilities/gen_case_tables.py from the Unicode Character Database %s
 * (%s). Do not edit it by hand: run the script again.
 * Unicode Data Files: Copyright (c) 1991-2024 Unicode, Inc., Unicode License v3
 * (https://www.unicode.org/license.txt).
 */
"""


def write_case_tables(data, special, version, out_path):
    simple = {"upper": [], "lower": [], "title": []}
    for code in sorted(data):
        category, upper, lower, title = data[code]
        if upper is not None and upper != code:
            simple["upper"].append((code, upper))
        if lower is not None and lower != code:
            simple["lower"].append((code, lower))
        # titlecase: listed only where it is not the uppercase mapping (the lookup falls back to it)
        if title is not None and title != (upper if upper is not None else code):
            simple["title"].append((code, title))

    # full mappings that differ from the simple ones: (code, offset into the sequence pool, length)
    pool = []
    full = {"upper": [], "lower": []}
    for code in sorted(special):
        lower, title, upper = special[code]
        for kind, seq in (("upper", upper), ("lower", lower)):
            if len(seq) == 1:
                continue   # the simple mapping covers it
            full[kind].append((code, len(pool), len(seq)))
            pool.extend(seq)

    words = ranges_of(c for c, entry in data.items() if entry[0][0] in "LMN")

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER % (version, "UnicodeData.txt, SpecialCasing.txt"))
        f.write("\n// Simple case mappings, sorted by code point: {code point, mapping}.\n")
        for kind in ("upper", "lower", "title"):
            f.write("static const CaseMapping case_mapping_%s[] = {\n" % kind)
            for i in range(0, len(simple[kind]), 6):
                f.write("\t" + " ".join("{0x%05X, 0x%05X}," % pair for pair in simple[kind][i:i + 6]) + "\n")
            f.write("};\n")
        f.write("\n// Unconditional full mappings of more than one code point (SpecialCasing.txt), sorted:\n")
        f.write("// {code point, first index into case_sequence_pool, length}.\n")
        f.write("static const char32_t case_sequence_pool[] = {\n%s\n};\n" % hex_list(pool, 10, 5))
        for kind in ("upper", "lower"):
            f.write("static const CaseSequence case_sequence_%s[] = {\n" % kind)
            for i in range(0, len(full[kind]), 6):
                f.write("\t" + " ".join("{0x%05X, %d, %d}," % entry for entry in full[kind][i:i + 6]) + "\n")
            f.write("};\n")
        f.write("\n// Letters, marks and numbers (general categories L, M, N): {first, last}, sorted.\n")
        f.write("static const CodepointRange word_character_ranges[] = {\n")
        for i in range(0, len(words), 6):
            f.write("\t" + " ".join("{0x%05X, 0x%05X}," % tuple(r) for r in words[i:i + 6]) + "\n")
        f.write("};\n")
    return {k: len(v) for k, v in simple.items()}, {k: len(v) for k, v in full.items()}, len(words)


def write_line_break_tables(runs, version, out_path):
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER % (version, "LineBreak.txt"))
        f.write("\n// UAX #14 line-break classes other than AL: {first, last, class}, sorted. The classes not\n")
        f.write("// told apart by the line breaker are AL (Utilities/gen_case_tables.py, LINE_BREAK_ALIASES).\n")
        f.write("static const LineBreakRange line_break_ranges[] = {\n")
        for i in range(0, len(runs), 4):
            f.write("\t" + " ".join("{0x%05X, 0x%05X, LineBreakClass::%s}," % (a, b, c) for a, b, c in runs[i:i + 4]) + "\n")
        f.write("};\n")
    return len(runs)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--ucd", help="directory holding UnicodeData.txt, SpecialCasing.txt and LineBreak.txt")
    parser.add_argument("--version", default="16.0.0", help="Unicode version to download (default 16.0.0)")
    args = parser.parse_args()

    paths = ucd_paths(args)
    data = unicode_data(paths["UnicodeData.txt"])
    special = special_casing(paths["SpecialCasing.txt"])
    simple, full, words = write_case_tables(data, special, args.version, os.path.join(SOURCE, "UnicodeCaseTables.inl"))
    runs = write_line_break_tables(line_break_classes(paths["LineBreak.txt"]), args.version, os.path.join(SOURCE, "UnicodeLineBreakTables.inl"))
    print("gen_case_tables: simple %s, full %s, %d word ranges, %d line-break ranges" % (simple, full, words, runs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
