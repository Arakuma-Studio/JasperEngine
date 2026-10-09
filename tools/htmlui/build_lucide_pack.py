#!/usr/bin/env python3
# Builds the engine's built-in Lucide icon pack (engine/libs/lucide/lucide.pack, phase-2
# §3.11.8) from a Lucide checkout at a release tag:
#
#   python3 tools/htmlui/build_lucide_pack.py <lucide checkout> <out.pack> [--version TAG]
#
# Python 3 standard library only. Reads icons/*.svg (the shapes) and icons/*.json (aliases),
# checks that every icon has Lucide's common root attributes (xmlns, width/height 24, viewBox
# 0 0 24 24, fill none, stroke currentColor, stroke-width 2, round caps and joins), strips
# them, and keeps each icon's inner shapes as compact markup (`<path d="..."/>`, attributes in
# source order, as Lucide's icon nodes keep them).
#
# Format (little-endian), read by htmlui::compat::LucideIcons and by
# tools/htmlui/render-reference.mjs:
#
#   "LUCP"                      4 bytes
#   u32 format version          1
#   u32 payload size            bytes of the inflated payload
#   zlib stream (RFC 1950)      the payload:
#     u32 icon count, u32 alias count
#     u16 length + bytes        the Lucide release tag ("1.53.0")
#     icon count x  { u16 name length, name, u32 markup offset, u32 markup length }
#                   sorted by name (byte order); names are kebab-case file names
#     alias count x { u16 name length, name, u32 icon index }, sorted by name
#     u32 markup size, markup   every icon's inner shapes, concatenated
#
# The output is deterministic for a given checkout (sorted tables, fixed compression level).

import argparse
import json
import os
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
import zlib

SVG_NS = "http://www.w3.org/2000/svg"
FORMAT_VERSION = 1

# Lucide's defaultAttributes (packages/lucide/src/defaultAttributes.ts); xmlns is the
# namespace ElementTree folds into the tag names
ROOT_ATTRIBUTES = {
    "width": "24",
    "height": "24",
    "viewBox": "0 0 24 24",
    "fill": "none",
    "stroke": "currentColor",
    "stroke-width": "2",
    "stroke-linecap": "round",
    "stroke-linejoin": "round",
}


def escape_attribute(value):
    return (value.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            .replace('"', "&quot;"))


def local_name(tag):
    return tag.split("}", 1)[1] if tag.startswith("{") else tag


def serialize(element):
    """An inner shape as `<tag a="v" .../>`, children serialized recursively."""
    name = local_name(element.tag)
    attributes = "".join(' %s="%s"' % (local_name(key), escape_attribute(value)) for key, value in element.attrib.items())
    children = list(element)
    if not children:
        return "<%s%s/>" % (name, attributes)
    return "<%s%s>%s</%s>" % (name, attributes, "".join(serialize(child) for child in children), name)


def read_icon(path):
    root = ET.parse(path).getroot()
    if root.tag != "{%s}svg" % SVG_NS:
        raise ValueError("%s: the root is not an SVG <svg> element" % path)
    attributes = dict(root.attrib)
    if attributes != ROOT_ATTRIBUTES:
        raise ValueError("%s: root attributes %r differ from Lucide's common ones" % (path, attributes))
    if (root.text or "").strip():
        raise ValueError("%s: text directly inside <svg>" % path)
    return "".join(serialize(child) for child in root)


def read_aliases(path):
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    names = []
    for alias in data.get("aliases", []):
        names.append(alias["name"] if isinstance(alias, dict) else alias)
    return names


def detect_version(checkout):
    try:
        result = subprocess.run(["git", "-C", checkout, "describe", "--tags", "--exact-match"],
                                capture_output=True, text=True, check=True)
        return result.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def build(checkout, version):
    icons_dir = os.path.join(checkout, "icons")
    names = sorted(name[:-4] for name in os.listdir(icons_dir) if name.endswith(".svg"))
    if not names:
        raise ValueError("%s has no icons" % icons_dir)

    markup = bytearray()
    icons = []
    aliases = {}
    for name in names:
        shapes = read_icon(os.path.join(icons_dir, name + ".svg")).encode("utf-8")
        icons.append((name.encode("utf-8"), len(markup), len(shapes)))
        markup += shapes
        for alias in read_aliases(os.path.join(icons_dir, name + ".json")):
            if alias in aliases and aliases[alias] != name:
                raise ValueError("alias %s names %s and %s" % (alias, aliases[alias], name))
            aliases[alias] = name

    index = {name: i for i, name in enumerate(names)}
    alias_rows = sorted((alias.encode("utf-8"), index[target]) for alias, target in aliases.items() if alias not in index)

    payload = bytearray()
    payload += struct.pack("<II", len(icons), len(alias_rows))
    version_bytes = version.encode("utf-8")
    payload += struct.pack("<H", len(version_bytes)) + version_bytes
    for name, offset, length in sorted(icons):
        payload += struct.pack("<H", len(name)) + name + struct.pack("<II", offset, length)
    for name, target in alias_rows:
        payload += struct.pack("<H", len(name)) + name + struct.pack("<I", target)
    payload += struct.pack("<I", len(markup)) + markup

    pack = bytearray(b"LUCP")
    pack += struct.pack("<II", FORMAT_VERSION, len(payload))
    pack += zlib.compress(bytes(payload), 9)
    return bytes(pack), len(icons), len(alias_rows), len(markup)


def main():
    parser = argparse.ArgumentParser(description="Build engine/libs/lucide/lucide.pack from a Lucide checkout.")
    parser.add_argument("checkout", help="Lucide checkout at a release tag")
    parser.add_argument("out", help="output .pack file")
    parser.add_argument("--version", default="", help="the Lucide release tag (default: git describe --tags --exact-match)")
    args = parser.parse_args()

    version = args.version or detect_version(args.checkout)
    if not version:
        print("cannot tell the Lucide release tag of %s; pass --version" % args.checkout, file=sys.stderr)
        return 1

    try:
        pack, icon_count, alias_count, markup_size = build(args.checkout, version)
    except (OSError, ValueError, ET.ParseError) as error:
        print("error: %s" % error, file=sys.stderr)
        return 1

    with open(args.out, "wb") as handle:
        handle.write(pack)
    print("%s: Lucide %s, %d icons, %d aliases, %d bytes of shapes, %d bytes packed"
          % (args.out, version, icon_count, alias_count, markup_size, len(pack)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
