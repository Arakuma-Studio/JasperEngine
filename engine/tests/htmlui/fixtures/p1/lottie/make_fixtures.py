#!/usr/bin/env python3
# Writes the Lottie fixtures of the vector tests (VectorTests.cpp, goldens G22a-c) next to this
# script: deterministic bytes (fixed ZIP timestamps), so rerunning it changes nothing.
#
#   shapes.json          200x200, 30 fps, 60 frames (2 s): a red box moving x 50 -> 150 and a
#                        circle whose fill goes blue -> green, both linear; markers
#                        "intro" [0, 30) and "loop" [30, 60)
#   stored_v1.lottie     dotLottie v1, stored entries: animations "first" and "second" (the
#                        active one), playback settings in the manifest
#   deflate_v2.lottie    dotLottie v2, deflated entries: a/shapes.json, initial.animation
#   image_v2.lottie      dotLottie v2 with an image layer whose PNG is i/dot.png (deflated)
#   image_ext.json       the same image animation as a plain .json; dot.png sits next to it
#   zip64.lottie         a ZIP64 archive (rejected)
#   encrypted.lottie     an entry with the "encrypted" flag (rejected)
#   bad_manifest.lottie  manifest.json without animations (rejected)
import json
import os
import struct
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
DATE = (2026, 10, 7, 12, 0, 0)


def linear(t0, s0, t1, s1):
    return {"a": 1, "k": [
        {"t": t0, "s": s0, "o": {"x": [0], "y": [0]}, "i": {"x": [1], "y": [1]}},
        {"t": t1, "s": s1},
    ]}


def static(v):
    return {"a": 0, "k": v}


def transform(p=(0, 0)):
    return {"ty": "tr", "p": static(list(p)), "a": static([0, 0]), "s": static([100, 100]),
            "r": static(0), "o": static(100)}


def layer_ks(position):
    return {"o": static(100), "r": static(0), "p": position, "a": static([0, 0, 0]),
            "s": static([100, 100, 100])}


def shapes_animation(name="shapes", rgb_box=(0.9, 0.2, 0.1)):
    box = {
        "ddd": 0, "ind": 1, "ty": 4, "nm": "box", "sr": 1, "ao": 0, "ip": 0, "op": 60, "st": 0, "bm": 0,
        "ks": layer_ks(linear(0, [50, 60, 0], 60, [150, 60, 0])),
        "shapes": [{"ty": "gr", "nm": "g", "it": [
            {"ty": "rc", "nm": "rect", "d": 1, "s": static([60, 60]), "p": static([0, 0]), "r": static(8)},
            {"ty": "fl", "nm": "fill", "c": static([rgb_box[0], rgb_box[1], rgb_box[2], 1]), "o": static(100), "r": 1},
            transform(),
        ]}],
    }
    circle = {
        "ddd": 0, "ind": 2, "ty": 4, "nm": "circle", "sr": 1, "ao": 0, "ip": 0, "op": 60, "st": 0, "bm": 0,
        "ks": layer_ks(static([100, 145, 0])),
        "shapes": [{"ty": "gr", "nm": "g", "it": [
            {"ty": "el", "nm": "ellipse", "d": 1, "s": static([70, 70]), "p": static([0, 0])},
            {"ty": "fl", "nm": "fill", "c": linear(0, [0.1, 0.3, 0.9, 1], 60, [0.1, 0.8, 0.2, 1]), "o": static(100), "r": 1},
            transform(),
        ]}],
    }
    return {
        "v": "5.7.4", "fr": 30, "ip": 0, "op": 60, "w": 200, "h": 200, "nm": name, "ddd": 0,
        "assets": [], "layers": [box, circle],
        "markers": [{"tm": 0, "cm": "intro", "dr": 30}, {"tm": 30, "cm": "loop", "dr": 30}],
    }


def png_rgba(width, height, rgba):
    raw = b"".join(b"\x00" + bytes(rgba) * width for _ in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def image_animation(folder):
    return {
        "v": "5.7.4", "fr": 30, "ip": 0, "op": 30, "w": 100, "h": 100, "nm": "image", "ddd": 0,
        "assets": [{"id": "img_0", "w": 100, "h": 100, "u": folder, "p": "dot.png", "e": 0}],
        "layers": [{"ddd": 0, "ind": 1, "ty": 2, "nm": "dot", "refId": "img_0", "sr": 1, "ao": 0,
                    "ip": 0, "op": 30, "st": 0, "bm": 0, "ks": layer_ks(static([0, 0, 0]))}],
    }


def dumps(obj):
    return json.dumps(obj, separators=(",", ":"), sort_keys=True).encode()


def write_zip(path, entries, method):
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in entries:
            info = zipfile.ZipInfo(name, DATE)
            info.compress_type = method
            info.create_system = 0
            archive.writestr(info, data)


def main():
    with open(os.path.join(HERE, "shapes.json"), "wb") as f:
        f.write(dumps(shapes_animation()))

    dot = png_rgba(100, 100, (255, 0, 0, 255))
    with open(os.path.join(HERE, "dot.png"), "wb") as f:
        f.write(dot)
    with open(os.path.join(HERE, "image_ext.json"), "wb") as f:
        f.write(dumps(image_animation("")))

    manifest_v1 = {
        "version": "1.0", "generator": "jasperengine tests",
        "animations": [
            {"id": "first", "speed": 1, "loop": False, "autoplay": False, "direction": 1, "playMode": "normal"},
            {"id": "second", "speed": 2, "loop": 3, "autoplay": True, "direction": -1, "playMode": "bounce"},
        ],
        "activeAnimationId": "second",
    }
    write_zip(os.path.join(HERE, "stored_v1.lottie"), [
        ("manifest.json", dumps(manifest_v1)),
        ("animations/first.json", dumps(shapes_animation("first", (0.9, 0.2, 0.1)))),
        ("animations/second.json", dumps(shapes_animation("second", (0.1, 0.2, 0.9)))),
    ], zipfile.ZIP_STORED)

    manifest_v2 = {"version": "2", "generator": "jasperengine tests",
                   "animations": [{"id": "shapes"}], "initial": {"animation": "shapes"}}
    write_zip(os.path.join(HERE, "deflate_v2.lottie"), [
        ("manifest.json", dumps(manifest_v2)),
        ("a/shapes.json", dumps(shapes_animation())),
    ], zipfile.ZIP_DEFLATED)

    manifest_image = {"version": "2", "animations": [{"id": "image"}]}
    write_zip(os.path.join(HERE, "image_v2.lottie"), [
        ("manifest.json", dumps(manifest_image)),
        ("a/image.json", dumps(image_animation("/i/"))),
        ("i/dot.png", dot),
    ], zipfile.ZIP_DEFLATED)

    # ZIP64: a zip64 end-of-central-directory record and locator, and the classic record's
    # fields set to 0xFFFF/0xFFFFFFFF as ZIP64 writers do
    zip64 = os.path.join(HERE, "zip64.lottie")
    write_zip(zip64, [("manifest.json", dumps(manifest_v2)), ("a/shapes.json", dumps(shapes_animation()))],
              zipfile.ZIP_STORED)
    data = open(zip64, "rb").read()
    eocd = data.rfind(b"PK\x05\x06")
    entries, cd_size, cd_offset = struct.unpack("<HII", data[eocd + 10:eocd + 20])
    record = struct.pack("<IQHHIIQQQQ", 0x06064b50, 44, 45, 45, 0, 0, entries, entries, cd_size, cd_offset)
    locator = struct.pack("<IIQI", 0x07064b50, 0, eocd, 1)
    classic = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0)
    open(zip64, "wb").write(data[:eocd] + record + locator + classic)

    # "encrypted": the general-purpose flag bit 0 set in the local and central headers
    encrypted = os.path.join(HERE, "encrypted.lottie")
    write_zip(encrypted, [("manifest.json", dumps(manifest_v2)), ("a/shapes.json", dumps(shapes_animation()))],
              zipfile.ZIP_STORED)
    data = bytearray(open(encrypted, "rb").read())
    for signature, flag_offset in ((b"PK\x03\x04", 6), (b"PK\x01\x02", 8)):
        start = 0
        while True:
            at = data.find(signature, start)
            if at < 0:
                break
            data[at + flag_offset] |= 0x01
            start = at + 4
    open(encrypted, "wb").write(bytes(data))

    write_zip(os.path.join(HERE, "bad_manifest.lottie"), [
        ("manifest.json", dumps({"version": "1.0", "animations": []})),
    ], zipfile.ZIP_STORED)


if __name__ == "__main__":
    main()
