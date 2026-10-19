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
    UnicodeJustificationTables.inl  'text-align: justify' (p6-justify): the characters Chromium
                                justifies around (Character::IsCjkIdeographOrSymbol: its lists, copied
                                below, and the emoji of emoji-data.txt and the RGI sequences), the
                                cursive scripts it does not space (Scripts.txt), the default-ignorable
                                code points (DerivedCoreProperties.txt) and the grapheme cluster breaks
                                (GraphemeBreakProperty.txt, Extended_Pictographic)

The Unicode Character Database files (UnicodeData.txt, SpecialCasing.txt, LineBreak.txt, Scripts.txt,
DerivedCoreProperties.txt, auxiliary/GraphemeBreakProperty.txt, emoji/emoji-data.txt and the emoji
sequence files emoji-sequences.txt, emoji-zwj-sequences.txt) are read from --ucd (one directory), or
downloaded from unicode.org into build/unicode-<version>/ of the repository.
The output is deterministic: run it again after a Unicode update and commit both files.
"""

import argparse
import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "Source", "Core")
# file name -> its path under https://www.unicode.org/Public/ (the emoji sequences are versioned without the micro version)
FILES = {
    "UnicodeData.txt": "%(v)s/ucd/UnicodeData.txt",
    "SpecialCasing.txt": "%(v)s/ucd/SpecialCasing.txt",
    "LineBreak.txt": "%(v)s/ucd/LineBreak.txt",
    "Scripts.txt": "%(v)s/ucd/Scripts.txt",
    "DerivedCoreProperties.txt": "%(v)s/ucd/DerivedCoreProperties.txt",
    "GraphemeBreakProperty.txt": "%(v)s/ucd/auxiliary/GraphemeBreakProperty.txt",
    "emoji-data.txt": "%(v)s/ucd/emoji/emoji-data.txt",
    "emoji-sequences.txt": "emoji/%(e)s/emoji-sequences.txt",
    "emoji-zwj-sequences.txt": "emoji/%(e)s/emoji-zwj-sequences.txt",
}

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
    emoji_version = ".".join(args.version.split(".")[:2])
    for name, location in FILES.items():
        path = os.path.join(cache, name)
        if not os.path.exists(path):
            url = "https://www.unicode.org/Public/" + location % {"v": args.version, "e": emoji_version}
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


# --- 'text-align: justify' (p6-justify) -------------------------------------------------------------

# Chromium 153's Character::IsCjkIdeographOrSymbol (third_party/blink/renderer/platform/text/
# character_property_data.h: kIsCjkIdeographOrSymbolArray and kIsCjkIdeographOrSymbolRanges, copied as
# they are), to which character_property_data_generator.cc adds [:Emoji_Presentation:] and the
# Extended_Pictographic code points of the RGI ZWJ and modifier sequences.
CJK_SYMBOL_VALUES = [
    0x2C7, 0x2CA, 0x2CB, 0x2D9, 0x2020, 0x2021, 0x2030, 0x203B, 0x203C, 0x2042, 0x2047, 0x2048,
    0x2049, 0x2051, 0x20DD, 0x20DE, 0x2100, 0x2103, 0x2105, 0x2109, 0x210A,
    0x2113, 0x2116, 0x2121, 0x212B, 0x213B, 0x2150, 0x2151, 0x2152, 0x217F,
    0x2189, 0x2307, 0x2312, 0x23CE, 0x2423, 0x25A0, 0x25A1, 0x25A2, 0x25AA,
    0x25AB, 0x25B1, 0x25B2, 0x25B3, 0x25B6, 0x25B7, 0x25BC, 0x25BD, 0x25C0,
    0x25C1, 0x25C6, 0x25C7, 0x25C9, 0x25CB, 0x25CC, 0x25EF, 0x2605, 0x2606,
    0x260E, 0x2616, 0x2617, 0x26A0, 0x2713, 0x271A, 0x273F, 0x2740, 0x2756,
    0x2763, 0x2B1A, 0xFE10, 0xFE11, 0xFE12, 0xFE19, 0xFF1D,
    0x1F100, 0x1F200, 0x1F237, 0x1F32C, 0x1F336, 0x1F37D, 0x1F43F, 0x1F54F,
    0x1F93B, 0x1F946,
]
CJK_SYMBOL_RANGES = [
    0x2E80, 0x2FDF, 0x31C0, 0x31EF, 0x3400, 0x4DBF, 0x4E00, 0x9FFF, 0xF900, 0xFAFF, 0x20000, 0x2FFFF,
    0x2156, 0x215A, 0x2160, 0x216B, 0x2170, 0x217B, 0x23BE, 0x23CC,
    0x2460, 0x2492, 0x249C, 0x24FF, 0x25CE, 0x25D3, 0x25E2, 0x25E6,
    0x2600, 0x2603, 0x2660, 0x266F,
    0x2672, 0x267D, 0x2776, 0x277F,
    0x2FF0, 0x302D, 0x3031, 0x312F,
    0x3190, 0x31BF,
    0x3200, 0x33FF,
    0x4DC0, 0x4DFF,
    0xF860, 0xF862,
    0xFE30, 0xFE6F,
    0xFF00, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1F, 0xFFEF,
    0x16FE0, 0x16FFF,
    0x17000, 0x187FF,
    0x18800, 0x18AFF,
    0x1B000, 0x1B0FF,
    0x1B100, 0x1B12F,
    0x1B170, 0x1B2FF,
    0x1F110, 0x1F129, 0x1F130, 0x1F149, 0x1F150, 0x1F169, 0x1F170, 0x1F189,
    0x1F202, 0x1F219, 0x1F21B, 0x1F22E, 0x1F230, 0x1F231, 0x1F23B, 0x1F24F,
    0x1F252, 0x1F2FF, 0x1F321, 0x1F32A, 0x1F394, 0x1F39F, 0x1F3CD, 0x1F3CE,
    0x1F3D4, 0x1F3DF, 0x1F3F1, 0x1F3F2, 0x1F3F5, 0x1F3F7, 0x1F4FD, 0x1F4FE,
    0x1F53E, 0x1F54A, 0x1F568, 0x1F573, 0x1F576, 0x1F579, 0x1F57B, 0x1F58F,
    0x1F591, 0x1F594, 0x1F597, 0x1F5A3, 0x1F5A5, 0x1F5E7, 0x1F5E9, 0x1F5FA,
    0x1F650, 0x1F67F, 0x1F6C6, 0x1F6CB, 0x1F6CD, 0x1F6CF, 0x1F6D3, 0x1F6D4,
    0x1F6D9, 0x1F6DB, 0x1F6E0, 0x1F6EA, 0x1F6ED, 0x1F6F3, 0x1F6FD, 0x1F6FF,
    0x1F900, 0x1F90B, 0x1FAC9, 0x1FACC,
]
# Character::IsCursiveScript: the scripts whose letters join (no letter spacing, no inter-character
# justification inside them)
CURSIVE_SCRIPTS = ("Arabic", "Hanifi_Rohingya", "Mandaic", "Mongolian", "Nko", "Phags_Pa", "Syriac")
# The grapheme cluster break classes the justification's cluster iteration tells apart (UAX #29);
# Extended_Pictographic is listed beside them (GB11)
GRAPHEME_CLASSES = ["Other", "Extend", "ZWJ", "SpacingMark", "Prepend", "Regional_Indicator", "L", "V", "T", "LV", "LVT", "ExtPict"]


def property_codes(path, wanted):
    """'code(..code) ; Property' lines -> the code points of the wanted property values"""
    codes = set()
    for line in read_lines(path):
        fields = [f.strip() for f in line.split(";")]
        if len(fields) < 2 or fields[1] not in wanted:
            continue
        code = fields[0]
        if ".." in code:
            a, b = code.split("..")
            codes.update(range(int(a, 16), int(b, 16) + 1))
        else:
            codes.add(int(code, 16))
    return codes


def property_classes(path, wanted):
    """'code(..code) ; Value' lines -> {code point: value} for the wanted values"""
    classes = {}
    for line in read_lines(path):
        fields = [f.strip() for f in line.split(";")]
        if len(fields) < 2 or fields[1] not in wanted:
            continue
        code = fields[0]
        if ".." in code:
            a, b = code.split("..")
            span = range(int(a, 16), int(b, 16) + 1)
        else:
            span = [int(code, 16)]
        for c in span:
            classes[c] = fields[1]
    return classes


def sequence_codes(path, kinds):
    """the code points of the emoji sequences of the given types ('cp cp ... ; type ; ...')"""
    codes = set()
    for line in read_lines(path):
        fields = [f.strip() for f in line.split(";")]
        if len(fields) < 2 or fields[1] not in kinds:
            continue
        for cp in fields[0].split():
            if ".." in cp:
                a, b = cp.split("..")
                codes.update(range(int(a, 16), int(b, 16) + 1))
            else:
                codes.add(int(cp, 16))
    return codes


def justification_tables(paths):
    ext_pict = property_codes(paths["emoji-data.txt"], ("Extended_Pictographic",))
    cjk = set(CJK_SYMBOL_VALUES)
    for i in range(0, len(CJK_SYMBOL_RANGES), 2):
        cjk.update(range(CJK_SYMBOL_RANGES[i], CJK_SYMBOL_RANGES[i + 1] + 1))
    cjk.update(property_codes(paths["emoji-data.txt"], ("Emoji_Presentation",)))
    sequences = sequence_codes(paths["emoji-zwj-sequences.txt"], ("RGI_Emoji_ZWJ_Sequence",))
    sequences |= sequence_codes(paths["emoji-sequences.txt"], ("RGI_Emoji_Modifier_Sequence",))
    cjk.update(c for c in sequences if c in ext_pict)
    cursive = property_codes(paths["Scripts.txt"], CURSIVE_SCRIPTS)
    ignorable = property_codes(paths["DerivedCoreProperties.txt"], ("Default_Ignorable_Code_Point",))
    grapheme = property_classes(paths["GraphemeBreakProperty.txt"], tuple(GRAPHEME_CLASSES[1:-1]))
    for c in ext_pict:
        grapheme.setdefault(c, "ExtPict")
    runs = []
    for c in sorted(grapheme):
        cls = grapheme[c]
        if runs and runs[-1][1] + 1 == c and runs[-1][2] == cls:
            runs[-1][1] = c
        else:
            runs.append([c, c, cls])
    return ranges_of(cjk), ranges_of(cursive), ranges_of(ignorable), runs


def write_justification_tables(tables, version, out_path):
    cjk, cursive, ignorable, grapheme = tables

    def write_ranges(f, name, ranges):
        f.write("static const CodepointRange %s[] = {\n" % name)
        for i in range(0, len(ranges), 6):
            f.write("\t" + " ".join("{0x%05X, 0x%05X}," % tuple(r) for r in ranges[i:i + 6]) + "\n")
        f.write("};\n")

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER % (version, "Scripts.txt, DerivedCoreProperties.txt, GraphemeBreakProperty.txt, emoji-data.txt, emoji-sequences.txt, "
                          "emoji-zwj-sequences.txt; Chromium 153's CJK lists"))
        f.write("\n// Chromium's Character::IsCjkIdeographOrSymbol: justified before and after each one ('text-justify: auto'). {first, last}, sorted.\n")
        write_ranges(f, "cjk_ideograph_or_symbol_ranges", cjk)
        f.write("\n// Chromium's Character::IsCursiveScript: %s (Scripts.txt).\n" % ", ".join(CURSIVE_SCRIPTS))
        write_ranges(f, "cursive_script_ranges", cursive)
        f.write("\n// Default_Ignorable_Code_Point (DerivedCoreProperties.txt).\n")
        write_ranges(f, "default_ignorable_ranges", ignorable)
        f.write("\n// Grapheme_Cluster_Break values other than Other, and Extended_Pictographic: {first, last, class}, sorted.\n")
        f.write("static const GraphemeBreakRange grapheme_break_ranges[] = {\n")
        for i in range(0, len(grapheme), 4):
            f.write("\t" + " ".join("{0x%05X, 0x%05X, GraphemeBreak::%s}," % (a, b, c) for a, b, c in grapheme[i:i + 4]) + "\n")
        f.write("};\n")
    return len(cjk), len(cursive), len(ignorable), len(grapheme)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--ucd", help="directory holding the Unicode files listed above")
    parser.add_argument("--version", default="16.0.0", help="Unicode version to download (default 16.0.0)")
    args = parser.parse_args()

    paths = ucd_paths(args)
    data = unicode_data(paths["UnicodeData.txt"])
    special = special_casing(paths["SpecialCasing.txt"])
    simple, full, words = write_case_tables(data, special, args.version, os.path.join(SOURCE, "UnicodeCaseTables.inl"))
    runs = write_line_break_tables(line_break_classes(paths["LineBreak.txt"]), args.version, os.path.join(SOURCE, "UnicodeLineBreakTables.inl"))
    justification = write_justification_tables(justification_tables(paths), args.version,
                                               os.path.join(SOURCE, "UnicodeJustificationTables.inl"))
    print("gen_case_tables: simple %s, full %s, %d word ranges, %d line-break ranges" % (simple, full, words, runs))
    print("gen_case_tables: justification: %d CJK, %d cursive, %d default-ignorable, %d grapheme ranges" % justification)
    return 0


if __name__ == "__main__":
    sys.exit(main())
