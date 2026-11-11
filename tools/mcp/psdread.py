#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""An independent, standard-library reader of Photoshop documents (PSD v1, PSB v2), written from the
Adobe Photoshop File Formats Specification and not from the engine's C++: the check S87's PSD
export is validated with (docs/specs/S87-title-localisation-interchange.md AC20).

    python3 tools/mcp/psdread.py <file.psd> [--check] [--icc]

It parses the header, the image resources (0x03ED resolution, 0x040F ICC profile checked against
§6.10's layout, 0x0421 version info), the layer and mask section (the layer records, their names
from `luni`, bounds, blend keys, opacity, clipping, visibility, `lsct` group nesting, `lyid`, the
user masks, channel data raw / PackBits / ZIP / ZIP with prediction, `Lr16` for 16-bit layers)
and the merged image, verifying every length against the file. Any inconsistency raises PsdError
naming the byte offset. As a module: read(bytes) -> dict, with each layer's planes decoded.
"""

from __future__ import annotations

import json
import struct
import sys
import zlib
from pathlib import Path


class PsdError(Exception):
    pass


class Reader:
    def __init__(self, data: bytes, start: int = 0, end: int | None = None):
        self.data = data
        self.pos = start
        self.end = len(data) if end is None else end

    def need(self, n: int, what: str):
        if n < 0 or self.pos + n > self.end:
            raise PsdError(f"{what}: {n} bytes needed at offset {self.pos}, {self.end - self.pos} left")

    def take(self, n: int, what: str = "data") -> bytes:
        self.need(n, what)
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out

    def u8(self): return self.take(1)[0]
    def u16(self): return struct.unpack(">H", self.take(2))[0]
    def i16(self): return struct.unpack(">h", self.take(2))[0]
    def u32(self): return struct.unpack(">I", self.take(4))[0]
    def i32(self): return struct.unpack(">i", self.take(4))[0]
    def u64(self): return struct.unpack(">Q", self.take(8))[0]

    def length(self, wide: bool, what: str) -> int:
        at = self.pos
        n = self.u64() if wide else self.u32()
        if self.pos + n > self.end:
            raise PsdError(f"{what} length {n} at offset {at} passes the end of its section ({self.end - self.pos} bytes left)")
        return n

    def sub(self, n: int, what: str) -> "Reader":
        self.need(n, what)
        r = Reader(self.data, self.pos, self.pos + n)
        self.pos += n
        return r

    def left(self) -> int:
        return self.end - self.pos


def unpackbits(src: bytes, size: int, at: int) -> bytes:
    out = bytearray()
    i = 0
    while i < len(src):
        n = src[i]
        i += 1
        if n < 128:
            out += src[i:i + n + 1]
            i += n + 1
        elif n > 128:
            if i >= len(src):
                raise PsdError(f"a PackBits run ends early at offset {at}")
            out += bytes([src[i]]) * (257 - n)
            i += 1
    if len(out) != size:
        raise PsdError(f"a PackBits row decodes to {len(out)} bytes, not {size}, at offset {at}")
    return bytes(out)


def decode_plane(r: Reader, w: int, h: int, bps: int, psb: bool, what: str) -> bytes:
    at = r.pos
    compression = r.u16()
    row = w * bps
    if w == 0 or h == 0:
        return b""
    if compression == 0:
        return r.take(row * h, what)
    if compression == 1:
        counts = [(r.u32() if psb else r.u16()) for _ in range(h)]
        return b"".join(unpackbits(r.take(c, what), row, r.pos) for c in counts)
    if compression in (2, 3):
        raw = r.take(r.left(), what)
        try:
            data = bytearray(zlib.decompress(raw))
        except zlib.error as e:
            raise PsdError(f"{what}: ZIP data at offset {at} does not inflate ({e})")
        if len(data) != row * h:
            raise PsdError(f"{what}: ZIP data at offset {at} inflates to {len(data)} bytes, not {row * h}")
        if compression == 3:
            for y in range(h):
                base = y * row
                if bps == 1:
                    for x in range(1, w):
                        data[base + x] = (data[base + x] + data[base + x - 1]) & 0xFF
                else:
                    prev = 0
                    for x in range(w):
                        v = (struct.unpack_from(">H", data, base + 2 * x)[0] + prev) & 0xFFFF
                        struct.pack_into(">H", data, base + 2 * x, v)
                        prev = v
        return bytes(data)
    raise PsdError(f"{what}: compression {compression} at offset {at} is unknown")


def unicode(r: Reader) -> str:
    n = r.u32()
    return r.take(2 * n, "a unicode string").decode("utf-16-be").rstrip("\0")


WIDE_KEYS = {"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"}


def check_icc(icc: bytes) -> dict:
    """§6.10: v2 display RGB/XYZ profile, desc / cprt / wtpt / r,g,bXYZ, one shared 1,024-entry curv."""
    if len(icc) < 132:
        raise PsdError("the ICC profile is shorter than its header")
    size = struct.unpack(">I", icc[:4])[0]
    if size != len(icc):
        raise PsdError(f"the ICC profile says {size} bytes, the resource holds {len(icc)}")
    if icc[36:40] != b"acsp" or icc[12:16] != b"mntr" or icc[16:20] != b"RGB " or icc[20:24] != b"XYZ ":
        raise PsdError("the ICC profile is not an RGB display profile with an XYZ PCS")
    version = struct.unpack(">I", icc[8:12])[0]
    count = struct.unpack(">I", icc[128:132])[0]
    tags = {}
    for i in range(count):
        sig, off, ln = struct.unpack(">4sII", icc[132 + 12 * i:144 + 12 * i])
        if off + ln > len(icc):
            raise PsdError(f"ICC tag {sig!r} passes the end of the profile")
        tags[sig.decode("latin-1")] = (off, ln)
    for need in ("desc", "cprt", "wtpt", "rXYZ", "gXYZ", "bXYZ", "rTRC", "gTRC", "bTRC"):
        if need not in tags:
            raise PsdError(f"the ICC profile has no {need} tag")
    if not (tags["rTRC"] == tags["gTRC"] == tags["bTRC"]):
        raise PsdError("the ICC TRC tags do not share one curve")
    off, _ = tags["rTRC"]
    if icc[off:off + 4] != b"curv" or struct.unpack(">I", icc[off + 8:off + 12])[0] != 1024:
        raise PsdError("the ICC TRC is not a 1,024-entry curv")
    doff, _ = tags["desc"]
    n = struct.unpack(">I", icc[doff + 8:doff + 12])[0]
    description = icc[doff + 12:doff + 12 + n].rstrip(b"\0").decode("latin-1")

    def xyz(tag):
        o, _ = tags[tag]
        return [struct.unpack(">i", icc[o + 8 + 4 * k:o + 12 + 4 * k])[0] / 65536 for k in range(3)]
    columns = [xyz("rXYZ"), xyz("gXYZ"), xyz("bXYZ")]
    white = [sum(c[k] for c in columns) for k in range(3)]
    return {"version": f"{version >> 24}.{(version >> 20) & 15}", "description": description, "tags": sorted(tags),
            "white": white, "size": len(icc)}


def read_layer_info(r: Reader, depth: int, psb: bool, pixels: bool) -> list[dict]:
    bps = depth // 8
    count = abs(r.i16())
    layers = []
    for index in range(count):
        at = r.pos
        top, left, bottom, right = r.i32(), r.i32(), r.i32(), r.i32()
        if bottom < top or right < left:
            raise PsdError(f"layer {index} at offset {at} has inverted bounds")
        nch = r.u16()
        channels = [(r.i16(), r.u64() if psb else r.u32()) for _ in range(nch)]
        if r.take(4) != b"8BIM":
            raise PsdError(f"layer {index}: no blend signature at offset {r.pos - 4}")
        layer = {"top": top, "left": left, "bottom": bottom, "right": right, "blend": r.take(4).decode("latin-1"),
                 "opacity": r.u8(), "clipping": r.u8() != 0}
        flags = r.u8()
        layer["visible"] = not (flags & 2)
        r.u8()
        extra = r.sub(r.length(False, f"layer {index} extra data"), "extra data")
        mlen = extra.length(False, f"layer {index} mask")
        mask = extra.sub(mlen, "mask")
        if mlen >= 18:
            layer["mask"] = {"top": mask.i32(), "left": mask.i32(), "bottom": mask.i32(), "right": mask.i32(), "default": mask.u8()}
        extra.sub(extra.length(False, f"layer {index} blending ranges"), "blending ranges")
        n = extra.u8()
        layer["pascal_name"] = extra.take(n).decode("latin-1")
        extra.take((4 - (n + 1) % 4) % 4)
        layer["keys"] = []
        layer["section"] = 0
        while extra.left() >= 12:
            sig = extra.take(4)
            if sig not in (b"8BIM", b"8B64"):
                raise PsdError(f"layer {index}: an additional block without its signature at offset {extra.pos - 4}")
            key = extra.take(4).decode("latin-1")
            ln = extra.length(psb and key in WIDE_KEYS, f"layer {index} block {key}")
            blk = extra.sub(ln, key)
            if ln % 2:
                extra.take(1)
            layer["keys"].append(key)
            if key == "luni":
                layer["name"] = unicode(blk)
            elif key == "lyid":
                layer["id"] = blk.u32()
            elif key in ("lsct", "lsdk"):
                layer["section"] = blk.u32()
            elif key == "iOpa":
                layer["fill_opacity"] = blk.u8()
        layer.setdefault("name", layer["pascal_name"])
        layer["channels"] = channels
        layers.append(layer)
    for layer in layers:
        w, h = layer["right"] - layer["left"], layer["bottom"] - layer["top"]
        planes = {}
        for cid, ln in layer.pop("channels"):
            ch = r.sub(ln, f"layer '{layer['name']}' channel {cid}")
            if ln < 2:
                raise PsdError(f"layer '{layer['name']}' channel {cid} has no compression field at offset {ch.pos}")
            cw, chh = (w, h)
            if cid == -2 and "mask" in layer:
                m = layer["mask"]
                cw, chh = m["right"] - m["left"], m["bottom"] - m["top"]
            plane = decode_plane(ch, cw, chh, bps, psb, f"layer '{layer['name']}' channel {cid}")
            if pixels:
                planes[cid] = plane
        layer["planes"] = planes
    return layers


def read(data: bytes, pixels: bool = True) -> dict:
    r = Reader(data)
    if r.take(4) != b"8BPS":
        raise PsdError("no '8BPS' signature at offset 0")
    version = r.u16()
    if version not in (1, 2):
        raise PsdError(f"version {version} at offset 4 is not 1 (PSD) or 2 (PSB)")
    psb = version == 2
    r.take(6)
    channels, height, width, depth, mode = r.u16(), r.u32(), r.u32(), r.u16(), r.u16()
    out = {"version": version, "channels": channels, "width": width, "height": height, "depth": depth, "mode": mode}
    r.sub(r.length(False, "color mode data"), "color mode data")
    res = r.sub(r.length(False, "image resources"), "image resources")
    out["resources"] = {}
    while res.left() >= 12:
        if res.take(4) != b"8BIM":
            raise PsdError(f"an image resource without '8BIM' at offset {res.pos - 4}")
        rid = res.u16()
        n = res.u8()
        res.take(n + (n + 1) % 2)
        ln = res.length(False, f"resource 0x{rid:04X}")
        body = res.take(ln)
        if ln % 2:
            res.take(1)
        out["resources"][f"0x{rid:04X}"] = len(body)
        if rid == 0x03ED:
            out["dpi"] = struct.unpack(">I", body[:4])[0] / 65536
        elif rid == 0x040F:
            # another tool's profile is fine to read; --icc requires §6.10's own
            try:
                out["icc"] = check_icc(body)
            except PsdError as e:
                out["icc"] = {"problem": str(e), "size": len(body)}
        elif rid == 0x0421:
            out["has_real_merged_data"] = body[4] != 0
    section = r.sub(r.length(psb, "layer and mask section"), "layer and mask section")
    layers: list[dict] = []
    if section.left():
        info = section.sub(section.length(psb, "layer info"), "layer info")
        if info.left():
            layers = read_layer_info(info, depth, psb, pixels)
        section.sub(section.length(False, "global layer mask"), "global layer mask")
        while section.left() >= 12:
            sig = section.take(4)
            if sig not in (b"8BIM", b"8B64"):
                raise PsdError(f"a document block without its signature at offset {section.pos - 4}")
            key = section.take(4).decode("latin-1")
            blk = section.sub(section.length(psb and key in WIDE_KEYS, f"document block {key}"), key)
            if key in ("Lr16", "Lr32") and not layers:
                layers = read_layer_info(blk, depth, psb, pixels)
    out["layers"] = layers
    # group nesting from the section dividers (file order: bottom first)
    stack: list[list] = [[]]
    for i, layer in enumerate(layers):
        if layer["section"] == 3:
            stack.append([])
        elif layer["section"] in (1, 2) and len(stack) > 1:
            children = stack.pop()
            stack[-1].append({"layer": i, "children": children})
        else:
            stack[-1].append({"layer": i, "children": []})
    if len(stack) != 1:
        raise PsdError("a group end marker has no group")
    out["tree"] = stack[0]
    at = r.pos
    compression = r.u16()
    bps = depth // 8
    rows = channels * height
    if compression == 0:
        merged = r.take(width * height * bps * channels, "merged image")
    elif compression == 1:
        counts = [(r.u32() if psb else r.u16()) for _ in range(rows)]
        merged = b"".join(unpackbits(r.take(c, "merged image"), width * bps, r.pos) for c in counts)
    else:
        raise PsdError(f"merged image compression {compression} at offset {at} is not raw or PackBits")
    if r.left():
        raise PsdError(f"{r.left()} bytes after the merged image at offset {r.pos}")
    out["merged"] = merged if pixels else len(merged)
    return out


def summary(psd: dict) -> dict:
    def node(n):
        layer = psd["layers"][n["layer"]]
        entry = {"name": layer["name"], "blend": layer["blend"], "opacity": layer["opacity"], "clipping": layer["clipping"],
                 "visible": layer["visible"], "bounds": [layer["left"], layer["top"], layer["right"], layer["bottom"]]}
        if n["children"] or layer["section"] in (1, 2):
            entry["children"] = [node(c) for c in n["children"]]
        return entry
    out = {k: psd[k] for k in ("version", "width", "height", "depth", "mode") if k in psd}
    out["dpi"] = psd.get("dpi")
    out["icc"] = psd.get("icc")
    out["tree"] = [node(n) for n in psd["tree"]]
    return out


def main(argv: list[str]) -> int:
    args = [a for a in argv if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        print("usage: psdread.py <file.psd> [--check] [--icc]", file=sys.stderr)
        return 2
    try:
        psd = read(Path(args[0]).read_bytes(), pixels="--check" not in argv)
    except (PsdError, OSError, struct.error) as e:
        print(f"psdread: {args[0]}: {e}", file=sys.stderr)
        return 1
    if "--icc" in argv and (not psd.get("icc") or "problem" in psd["icc"]):
        print(f"psdread: {args[0]}: " + (psd["icc"]["problem"] if psd.get("icc") else "no ICC profile (resource 0x040F)"), file=sys.stderr)
        return 1
    if "--check" in argv:
        print(f"{args[0]}: ok, {len(psd['layers'])} layer records")
        return 0
    print(json.dumps(summary(psd), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
