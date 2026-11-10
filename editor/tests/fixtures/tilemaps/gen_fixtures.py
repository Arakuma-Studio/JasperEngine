#!/usr/bin/env python3
# (c) JasperDev and contributors
# SPDX-License-Identifier: MIT
"""Writes the Tiled fixtures whose tile data is encoded or compressed, so their bytes are
reproducible (docs/specs/S50-grid-tilemap.md AC8):

  ortho_<csv|base64|zlib|gzip|zstd>.tmj / .tmx   one 4x3 orthogonal map, five encodings
  infinite_zlib.tmj, infinite_csv.tmx            chunked (infinite) layers
  <orientation>_*.tmj / .tmx                     every orientation; each cell holds its own tile

Run from anywhere; needs python3 and the zstd command line tool (for the zstd fixture). The
hand-written fixtures next to these (features, wang, external_*, collection, *.ldtk, ...) and every
*.expected.json are not generated.
"""

import base64
import gzip
import io
import json
import struct
import subprocess
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent

H, V, D = 1 << 31, 1 << 30, 1 << 29
# firstgid 1; 0 empty; the same 4x3 map in every encoding
GIDS = [1, 2, 0, 3 | H,
        4 | V, 5 | D, 6 | H | V | D, 0,
        7, 0, 8, 9]


def gzip_bytes(data: bytes) -> bytes:
    # mtime fixed so the bytes do not depend on when this ran
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as f:
        f.write(data)
    return buf.getvalue()


def zstd_bytes(data: bytes) -> bytes:
    return subprocess.run(["zstd", "-q", "-c", "--no-check"], input=data, stdout=subprocess.PIPE, check=True).stdout


def encode(gids, encoding):
    raw = struct.pack("<%dI" % len(gids), *gids)
    if encoding == "csv":
        return None
    payload = {"base64": raw, "zlib": zlib.compress(raw), "gzip": gzip_bytes(raw), "zstd": zstd_bytes(raw)}[encoding]
    return base64.b64encode(payload).decode("ascii")


COMPRESSION = {"csv": None, "base64": None, "zlib": "zlib", "gzip": "gzip", "zstd": "zstd"}
TILESET_JSON = {"firstgid": 1, "name": "tiles", "tilewidth": 16, "tileheight": 16, "tilecount": 9, "columns": 3,
                "image": "images/tiles.png", "imagewidth": 48, "imageheight": 48}
TILESET_XML = '<tileset firstgid="1" name="tiles" tilewidth="16" tileheight="16" tilecount="9" columns="3">\n  <image source="images/tiles.png" width="48" height="48"/>\n </tileset>'


def tmj_map(extra, layers, tilesets=None, width=4, height=3, tw=16, th=16):
    doc = {"type": "map", "orientation": "orthogonal", "width": width, "height": height, "tilewidth": tw, "tileheight": th,
           "infinite": False, "tilesets": tilesets if tilesets is not None else [TILESET_JSON], "layers": layers}
    doc.update(extra)
    return doc


def write_json(name, doc):
    (HERE / name).write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8")


def tmx_header(extra="", width=4, height=3, tw=16, th=16, tilesets=TILESET_XML, orientation="orthogonal"):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<map version="1.10" orientation="{orientation}" width="{width}" height="{height}" '
            f'tilewidth="{tw}" tileheight="{th}"{extra}>\n {tilesets}\n')


def csv_lines(gids, width):
    rows = [",".join(str(g) for g in gids[i:i + width]) for i in range(0, len(gids), width)]
    return "\n" + ",\n".join(rows) + "\n"


def main():
    # the same map in every encoding, both formats
    for encoding in ("csv", "base64", "zlib", "gzip", "zstd"):
        layer = {"type": "tilelayer", "name": "Ground", "width": 4, "height": 3}
        if encoding == "csv":
            layer["data"] = GIDS
        else:
            layer["data"] = encode(GIDS, encoding)
            layer["encoding"] = "base64"
            if COMPRESSION[encoding]:
                layer["compression"] = COMPRESSION[encoding]
        write_json(f"ortho_{encoding}.tmj", tmj_map({}, [layer]))

        attr = 'encoding="csv"' if encoding == "csv" else 'encoding="base64"' + (f' compression="{COMPRESSION[encoding]}"' if COMPRESSION[encoding] else "")
        text = csv_lines(GIDS, 4) if encoding == "csv" else "\n" + encode(GIDS, encoding) + "\n"
        (HERE / f"ortho_{encoding}.tmx").write_text(tmx_header() + f' <layer id="1" name="Ground" width="4" height="3">\n  <data {attr}>{text}</data>\n </layer>\n</map>\n', encoding="utf-8")

    # infinite maps: two chunks at the map's negative and positive coordinates (Tiled's chunk
    # size is 16; 4x2 chunks keep the fixture small)
    chunk_a = [0, 1, 0, 2,
               3, 0, 4 | H, 0]
    chunk_b = [5, 0, 0, 6]
    chunks = [{"x": -4, "y": -2, "width": 4, "height": 2, "data": encode(chunk_a, "zlib")},
              {"x": 8, "y": 3, "width": 2, "height": 2, "data": encode(chunk_b, "zlib")}]
    write_json("infinite_zlib.tmj", tmj_map({"infinite": True, "width": 16, "height": 16}, [
        {"type": "tilelayer", "name": "Ground", "width": 4, "height": 2, "startx": -4, "starty": -2, "x": 0, "y": 0,
         "encoding": "base64", "compression": "zlib", "chunks": chunks}]))
    (HERE / "infinite_csv.tmx").write_text(
        tmx_header(' infinite="1"') +
        f' <layer id="1" name="Ground" width="4" height="2">\n  <data encoding="csv">\n'
        f'   <chunk x="-4" y="-2" width="4" height="2">{csv_lines(chunk_a, 4)}</chunk>\n'
        f'   <chunk x="8" y="3" width="2" height="2">{csv_lines(chunk_b, 2)}</chunk>\n'
        f'  </data>\n </layer>\n</map>\n', encoding="utf-8")

    # every orientation: gid = 1 + x + y * W, so the importer test can read back Tiled's (x, y)
    # from the tile id and check the pixel positions
    orientations = [
        ("iso_w4h3", "isometric", {}, 4, 3, 32, 16),
        ("iso_w3h4", "isometric", {}, 3, 4, 32, 16),
        ("staggered_y_odd_h3", "staggered", {"staggeraxis": "y", "staggerindex": "odd"}, 4, 3, 32, 16),
        ("staggered_y_odd_h4", "staggered", {"staggeraxis": "y", "staggerindex": "odd"}, 3, 4, 32, 16),
        ("staggered_y_even_h3", "staggered", {"staggeraxis": "y", "staggerindex": "even"}, 4, 3, 32, 16),
        ("staggered_y_even_h4", "staggered", {"staggeraxis": "y", "staggerindex": "even"}, 3, 4, 32, 16),
        ("staggered_x_odd", "staggered", {"staggeraxis": "x", "staggerindex": "odd"}, 4, 3, 32, 16),
        ("staggered_x_even", "staggered", {"staggeraxis": "x", "staggerindex": "even"}, 3, 4, 32, 16),
        ("hex_y_odd_h3", "hexagonal", {"staggeraxis": "y", "staggerindex": "odd", "hexsidelength": 14}, 4, 3, 32, 28),
        ("hex_y_odd_h4", "hexagonal", {"staggeraxis": "y", "staggerindex": "odd", "hexsidelength": 14}, 3, 4, 32, 28),
        ("hex_y_even_h3", "hexagonal", {"staggeraxis": "y", "staggerindex": "even", "hexsidelength": 14}, 4, 3, 32, 28),
        ("hex_y_even_h4", "hexagonal", {"staggeraxis": "y", "staggerindex": "even", "hexsidelength": 14}, 3, 4, 32, 28),
        ("hex_x_odd", "hexagonal", {"staggeraxis": "x", "staggerindex": "odd", "hexsidelength": 14}, 4, 3, 28, 32),
        ("hex_x_even", "hexagonal", {"staggeraxis": "x", "staggerindex": "even", "hexsidelength": 14}, 3, 4, 28, 32),
    ]
    for name, orientation, extra, width, height, tw, th in orientations:
        count = width * height
        gids = [1 + i for i in range(count)]
        tileset = {"firstgid": 1, "name": "cells", "tilewidth": tw, "tileheight": th, "tilecount": count, "columns": width,
                   "image": "images/cells.png", "imagewidth": width * tw, "imageheight": height * th}
        layer = {"type": "tilelayer", "name": "Cells", "width": width, "height": height, "data": gids}
        write_json(f"{name}.tmj", tmj_map(dict(orientation=orientation, **extra), [layer], [tileset], width, height, tw, th))
        attrs = "".join(f' {k}="{v}"' for k, v in extra.items())
        tileset_xml = (f'<tileset firstgid="1" name="cells" tilewidth="{tw}" tileheight="{th}" tilecount="{count}" columns="{width}">\n'
                       f'  <image source="images/cells.png" width="{width * tw}" height="{height * th}"/>\n </tileset>')
        (HERE / f"{name}.tmx").write_text(
            tmx_header(attrs, width, height, tw, th, tileset_xml, orientation) +
            f' <layer id="1" name="Cells" width="{width}" height="{height}">\n  <data encoding="csv">{csv_lines(gids, width)}</data>\n </layer>\n</map>\n',
            encoding="utf-8")

    # the hex map's bit 28 and bit 29 (rotation): reported, imported unrotated
    gids = [1 | (1 << 28), 2 | D, 3, 4]
    write_json("hex_rotation.tmj", tmj_map({"orientation": "hexagonal", "staggeraxis": "y", "staggerindex": "odd", "hexsidelength": 14},
                                           [{"type": "tilelayer", "name": "Hex", "width": 2, "height": 2, "data": gids}],
                                           [{"firstgid": 1, "name": "cells", "tilewidth": 32, "tileheight": 28, "tilecount": 4, "columns": 2,
                                             "image": "images/cells.png", "imagewidth": 64, "imageheight": 56}], 2, 2, 32, 28))


if __name__ == "__main__":
    main()
