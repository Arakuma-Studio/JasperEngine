#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Deterministic local HTTP server for the HTML UI importer tests (phase-2 §5.4).

    fixture_server.py --root engine/tests/htmlui/fixtures/compat/net --port 0

Prints the port it listens on (127.0.0.1) as the first line of stdout, then serves until it
is killed. Python 3 standard library only. The tests map the real hosts onto it with
HttpClient::setHostRewrite:

    https://fonts.googleapis.com         -> /gf        Google Fonts css2, css and icon APIs
    https://fonts.gstatic.com            -> /gstatic   static TTFs per family/weight/style
    https://fonts.google.com/download    -> /gfdl/download   license list ()]}'-guarded JSON)
    https://fonts.google.com/metadata    -> /gfmeta/metadata  family list
    https://cdnjs.cloudflare.com         -> /cdn       Font-Awesome-like CSS, woff2-only CSS, @import chains
    https://images.example.com           -> /img       images by content type and magic bytes; videos
                                                    (/img/clip.webm: the AV1 test card, /img/trailer:
                                                    the VP9 one as video/mp4, phase 6)
    https://lottie.host                  -> /lottie    Lottie JSON, dotLottie v1/v2 (built here)
    https://lottiefiles.com              -> /lfpage    a LottieFiles page linking an animation

Errors: /status/404, /status/500, /slow (sleeps past the client's timeout), /redirect
(302 to /img/placeholder). /__requests lists the requests seen ({path, ua}) for the tests.
"""

import argparse
import io
import json
import os
import struct
import sys
import threading
import time
import urllib.parse
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# family -> {"variants": [(weight, italic)], "license": file name or None (download list fails)}
FAMILIES = {
    "Inter": {"variants": [(w, i) for i in (False, True) for w in range(100, 1000, 100)], "license": "OFL.txt"},
    "Orbitron": {"variants": [(w, False) for w in range(400, 1000, 100)], "license": "OFL.txt"},
    "Press Start 2P": {"variants": [(400, False)], "license": "OFL.txt"},
    "Roboto Mono": {"variants": [(w, i) for i in (False, True) for w in range(100, 800, 100)], "license": "LICENSE.txt"},
    "Karla": {"variants": [(w, i) for i in (False, True) for w in range(200, 900, 100)], "license": "OFL.txt"},
    "Material Icons": {"variants": [(400, False)], "license": "LICENSE.txt"},
    "Unlicensed Sans": {"variants": [(400, False)], "license": None},
}

REQUESTS = []
REQUESTS_LOCK = threading.Lock()


def slug(family):
    return family.lower().replace(" ", "")


def family_by_slug(value):
    for name in FAMILIES:
        if slug(name) == value:
            return name
    return None


def patch_font(base, weight, italic):
    """The base TTF with OS/2 usWeightClass/fsSelection and head macStyle set, checksums fixed."""
    data = bytearray(base)
    count = struct.unpack(">H", data[4:6])[0]
    tables = {}
    for i in range(count):
        entry = 12 + 16 * i
        tag = bytes(data[entry:entry + 4]).decode("latin-1")
        offset, length = struct.unpack(">II", data[entry + 8:entry + 16])
        tables[tag] = (entry, offset, length)

    def checksum(chunk):
        padded = bytes(chunk) + b"\0" * ((4 - len(chunk) % 4) % 4)
        return sum(struct.unpack(">%dI" % (len(padded) // 4), padded)) & 0xFFFFFFFF

    _, os2, _ = tables["OS/2"]
    struct.pack_into(">H", data, os2 + 4, weight)
    selection = struct.unpack(">H", data[os2 + 62:os2 + 64])[0] & ~0x61
    selection |= 0x01 if italic else 0
    selection |= 0x20 if weight >= 700 else 0
    selection |= 0x40 if not italic and weight < 700 else 0
    struct.pack_into(">H", data, os2 + 62, selection)
    _, head, _ = tables["head"]
    style = (0x01 if weight >= 700 else 0) | (0x02 if italic else 0)
    struct.pack_into(">H", data, head + 44, style)
    struct.pack_into(">I", data, head + 8, 0)
    for tag in ("OS/2", "head"):
        entry, offset, length = tables[tag]
        struct.pack_into(">I", data, entry + 4, checksum(data[offset:offset + length]))
    struct.pack_into(">I", data, head + 8, (0xB1B0AFBA - checksum(data)) & 0xFFFFFFFF)
    return bytes(data)


def parse_variant(text):
    text = text.strip().lower()
    if text in ("", "regular", "r", "normal"):
        return (400, False)
    if text in ("italic", "i"):
        return (400, True)
    if text in ("bold", "b"):
        return (700, False)
    if text in ("bolditalic", "bi"):
        return (700, True)
    digits = "".join(c for c in text if c.isdigit())
    rest = text[len(digits):]
    if not digits or rest not in ("", "i", "italic"):
        return None
    return (int(digits), bool(rest))


def css2_request(value):
    """css2 family parameter -> (family, [(weight, italic)], [(min, max, italic)]) or None."""
    name, _, spec = value.partition(":")
    variants, ranges = [], []
    if not spec:
        return name, [(400, False)], ranges
    axes, _, tuples = spec.partition("@")
    axes = axes.split(",")
    for item in tuples.split(";"):
        values = item.split(",")
        if len(values) != len(axes):
            return None
        weight, italic, weight_range = 400, False, None
        for axis, val in zip(axes, values):
            if axis == "ital":
                italic = val == "1"
            elif axis == "wght":
                if ".." in val:
                    low, high = val.split("..")
                    weight_range = (int(low), int(high))
                else:
                    weight = int(val)
        if weight_range:
            ranges.append((weight_range[0], weight_range[1], italic))
        else:
            variants.append((weight, italic))
    return name, variants, ranges


class Handler(BaseHTTPRequestHandler):
    server_version = "JasperFixture/1.0"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quiet
        pass

    # ---- helpers

    def send(self, status, body, content_type, headers=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def file(self, relative, content_type):
        path = os.path.join(self.server.root, relative)
        if not os.path.isfile(path):
            return self.send(404, "not found", "text/plain")
        with open(path, "rb") as handle:
            self.send(200, handle.read(), content_type)

    def video(self, name, content_type):
        # the clips of engine/tests/htmlui/fixtures/video (the root is fixtures/compat/net)
        path = os.path.join(self.server.root, "..", "..", "video", name)
        with open(path, "rb") as handle:
            self.send(200, handle.read(), content_type)

    def base_font(self):
        with open(os.path.join(self.server.root, "gstatic", "base.ttf"), "rb") as handle:
            return handle.read()

    # ---- routes

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        path = url.path
        query = url.query
        ua = self.headers.get("User-Agent", "")
        if path not in ("/__requests", "/__reset"):
            with REQUESTS_LOCK:
                REQUESTS.append({"path": self.path, "ua": ua})

        if path == "/__requests":
            with REQUESTS_LOCK:
                return self.send(200, json.dumps(REQUESTS), "application/json")
        if path == "/__reset":
            with REQUESTS_LOCK:
                REQUESTS.clear()
            return self.send(200, "ok", "text/plain")

        if path in ("/gf/css2", "/gf/css", "/gf/icon"):
            return self.google_css(path, query, ua)
        if path.startswith("/gstatic/"):
            return self.google_font(path)
        if path == "/gfdl/download/list":
            return self.google_license(query)
        if path == "/gfmeta/metadata/fonts":
            return self.google_metadata()

        if path.startswith("/cdn/"):
            relative = path[len("/cdn/"):]
            if relative.endswith(".ttf"):
                return self.send(200, self.base_font(), "font/ttf")
            if relative.endswith(".woff2"):
                with open(os.path.join(self.server.root, "..", "..", "fonts", "webfonts", "webfont.woff2"), "rb") as font:
                    return self.send(200, font.read(), "font/woff2")
            if relative.endswith(".woff"):
                with open(os.path.join(self.server.root, "..", "..", "fonts", "webfonts", "webfont.woff"), "rb") as font:
                    return self.send(200, font.read(), "font/woff")
            if relative.endswith(".css"):
                return self.file(os.path.join("cdn", relative), "text/css; charset=utf-8")
            if relative.endswith(".png"):
                return self.file(os.path.join("img", "pixel.png"), "image/png")
            return self.send(404, "not found", "text/plain")

        if path == "/img/photo":
            return self.file(os.path.join("img", "photo.jpg"), "image/jpeg")
        if path == "/img/placeholder":
            return self.file(os.path.join("img", "placeholder.svg"), "image/svg+xml")
        if path == "/img/webp":
            return self.file(os.path.join("img", "image.webp"), "image/webp")
        if path == "/img/octet":
            return self.file(os.path.join("img", "pixel.png"), "application/octet-stream")
        if path == "/img/clip.webm":
            return self.video("testcard-noaudio.webm", "video/webm")
        if path == "/img/trailer":
            return self.video("testcard-vp9.webm", "video/mp4")
        if path == "/img/page":
            return self.send(200, "<!doctype html><html><body>not an image</body></html>", "text/html; charset=utf-8")

        if path == "/lottie/anim.json":
            return self.file(os.path.join("lottie", "anim.json"), "application/json")
        if path.startswith("/lottie/images/"):
            return self.file(os.path.join("lottie", "images", os.path.basename(path)), "image/png")
        if path in ("/lottie/anim-v1.lottie", "/lottie/anim-v2.lottie"):
            return self.send(200, self.dotlottie(2 if "v2" in path else 1), "application/zip")
        if path.startswith("/lfpage/"):
            return self.file(os.path.join("lfpage", "animation.html"), "text/html; charset=utf-8")

        if path == "/status/404":
            return self.send(404, "<html>missing</html>", "text/html")
        if path == "/status/500":
            return self.send(500, "<html>broken</html>", "text/html")
        if path == "/slow":
            time.sleep(float(urllib.parse.parse_qs(query).get("s", ["6"])[0]))
            return self.send(200, "late", "text/plain")
        if path == "/redirect":
            return self.send(302, "", "text/plain", {"Location": "/img/placeholder"})
        return self.send(404, "not found", "text/plain")

    # ---- Google Fonts

    def google_css(self, path, query, ua):
        # a browser asks for WOFF2 subsets: the importer must never look like one
        browser = "Mozilla" in ua
        faces = []
        for value in urllib.parse.parse_qs(query, keep_blank_values=True).get("family", []):
            requests = []
            if path == "/gf/css2":
                parsed = css2_request(value)
                if parsed is None:
                    return self.send(400, "<html>bad request</html>", "text/html")
                requests.append(parsed)
            else:
                for item in value.split("|"):
                    name, _, spec = item.partition(":")
                    variants = [parse_variant(v) for v in spec.split(",")] if spec else [(400, False)]
                    if None in variants:
                        return self.send(400, "<html>bad request</html>", "text/html")
                    requests.append((name, variants, []))
            for name, variants, ranges in requests:
                if name not in FAMILIES:
                    return self.send(400, "<!DOCTYPE html><html><body>unknown family</body></html>", "text/html")
                offered = FAMILIES[name]["variants"]
                chosen = []
                for variant in variants:
                    if variant in offered:
                        chosen.append(variant)
                    elif path == "/gf/css2":
                        return self.send(400, "<!DOCTYPE html><html><body>unknown style</body></html>", "text/html")
                for low, high, italic in ranges:
                    chosen += [v for v in offered if low <= v[0] <= high and v[1] == italic]
                for variant in sorted(set(chosen), key=lambda v: (v[1], v[0])):
                    faces.append((name, variant[0], variant[1]))
        if not faces:
            return self.send(400, "<html>no family</html>", "text/html")
        css = []
        for name, weight, italic in faces:
            file = "%d%s" % (weight, "i" if italic else "")
            if browser:
                src = "url(https://fonts.gstatic.com/%s/%s.woff2) format('woff2')" % (slug(name), file)
            else:
                src = "url(https://fonts.gstatic.com/%s/%s.ttf) format('truetype')" % (slug(name), file)
            css.append("@font-face {\n  font-family: '%s';\n  font-style: %s;\n  font-weight: %d;\n  font-display: swap;\n  src: %s;\n}\n"
                       % (name, "italic" if italic else "normal", weight, src))
        if path == "/gf/icon":
            css.append(".material-icons {\n  font-family: 'Material Icons';\n  font-size: 24px;\n}\n")
        return self.send(200, "".join(css), "text/css; charset=utf-8")

    def google_font(self, path):
        parts = path.split("/")
        if len(parts) != 4:
            return self.send(404, "not found", "text/plain")
        family = family_by_slug(parts[2])
        name, extension = os.path.splitext(parts[3])
        italic = name.endswith("i")
        weight = int(name.rstrip("i")) if name.rstrip("i").isdigit() else 0
        if family is None or (weight, italic) not in FAMILIES[family]["variants"]:
            return self.send(404, "not found", "text/plain")
        if extension == ".woff2":
            return self.send(200, b"wOF2" + b"\0" * 60, "font/woff2")
        return self.send(200, patch_font(self.base_font(), weight, italic), "font/ttf")

    def google_license(self, query):
        family = urllib.parse.parse_qs(query).get("family", [""])[0]
        if family not in FAMILIES:
            return self.send(400, "<html>unknown</html>", "text/html")
        license_file = FAMILIES[family]["license"]
        if license_file is None:
            return self.send(500, "<html>error</html>", "text/html")
        with open(os.path.join(self.server.root, "gstatic", "OFL.txt"), "r", encoding="utf-8") as handle:
            text = handle.read()
        body = {
            "zipName": family.replace(" ", "_") + ".zip",
            "manifest": {
                "files": [{"filename": license_file, "contents": "%s\n\n%s" % (family, text)}],
                "fileRefs": [{"filename": "static/%s-Regular.ttf" % family.replace(" ", ""),
                              "url": "https://fonts.gstatic.com/%s/400.ttf" % slug(family)}],
            },
        }
        return self.send(200, ")]}'\n" + json.dumps(body, indent=2), "application/json; charset=utf-8")

    def google_metadata(self):
        families = []
        for name, info in sorted(FAMILIES.items()):
            fonts = {"%d%s" % (w, "i" if i else ""): {"thickness": 5} for w, i in info["variants"]}
            families.append({"family": name, "category": "Sans Serif", "fonts": fonts})
        return self.send(200, json.dumps({"familyMetadataList": families}), "application/json")

    # ---- dotLottie

    def dotlottie(self, version):
        with open(os.path.join(self.server.root, "lottie", "anim.json"), "r", encoding="utf-8") as handle:
            animation = json.load(handle)
        with open(os.path.join(self.server.root, "lottie", "images", "img_0.png"), "rb") as handle:
            image = handle.read()
        folder, images = ("a", "i") if version == 2 else ("animations", "images")
        for asset in animation.get("assets", []):
            if "p" in asset:
                asset["u"] = "/%s/" % images
        if version == 2:
            manifest = {"version": "2", "generator": "fixture", "animations": [{"id": "intro"}, {"id": "main"}],
                        "initial": {"animation": "main"}}
        else:
            manifest = {"version": "1.0", "generator": "fixture", "animations": [{"id": "intro"}, {"id": "main", "loop": True}],
                        "activeAnimationId": "main"}
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            stamp = (2026, 1, 1, 0, 0, 0)
            def add(name, data, method):
                info = zipfile.ZipInfo(name, stamp)
                info.compress_type = method
                archive.writestr(info, data)
            add("manifest.json", json.dumps(manifest), zipfile.ZIP_STORED)
            add("%s/intro.json" % folder, json.dumps({"v": "5.7.0", "fr": 30, "ip": 0, "op": 10, "w": 10, "h": 10, "layers": []}),
                zipfile.ZIP_DEFLATED)
            add("%s/main.json" % folder, json.dumps(animation), zipfile.ZIP_DEFLATED)
            add("%s/img_0.png" % images, image, zipfile.ZIP_STORED)
        return buffer.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", required=True, help="fixture directory (engine/tests/htmlui/fixtures/compat/net)")
    parser.add_argument("--port", type=int, default=0, help="0 = any free port")
    args = parser.parse_args()

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.daemon_threads = True
    server.root = os.path.abspath(args.root)
    print(server.server_address[1], flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
