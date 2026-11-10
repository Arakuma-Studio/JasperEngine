"""PNG comparison for the MCP golden-image tests (phase-4 spec 3.9), pure Python, standard
library only.

An image is a tuple (width, height, rgba) with rgba a bytearray of width * height * 4 bytes,
rows top to bottom.

    decode(png_bytes) -> img          8-bit RGB/RGBA (also grey and grey+alpha), non-interlaced,
                                      the five filters (None, Sub, Up, Average, Paeth)
    encode(img) or encode(w, h, rgba) -> png bytes (RGBA, filter None)
    crop(img, x, y, w, h) -> img
    downscale(img, w, h) -> img       box filter (each output pixel averages the source area it covers)
    compare(a, b, channel_tolerance=24, max_fraction=0.01) -> (ok, fraction, diff_png)
    flat(img) -> bool                 every pixel the same (within `tolerance`, default 2)

Command line: python3 pngdiff.py a.png b.png [--size 640x360] [--tolerance 24]
[--max-fraction 0.01] [--diff out.png]; exit 0 when the images match.
"""

from __future__ import annotations

import struct
import sys
import zlib

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# bytes per pixel of the 8-bit colour types
_CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}


def decode(data: bytes):
    """(width, height, rgba bytearray) of an 8-bit PNG."""
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("not a PNG")
    pos, width, height, color_type, idat = 8, 0, 0, None, []
    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind, chunk = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", chunk[:13])
            if depth != 8:
                raise ValueError(f"bit depth {depth} (only 8 is supported)")
            if color_type not in _CHANNELS:
                raise ValueError(f"colour type {color_type} (palette images are not supported)")
            if interlace:
                raise ValueError("interlaced PNG")
        elif kind == b"IDAT":
            idat.append(chunk)
        elif kind == b"IEND":
            break
        pos += 12 + length
    if color_type is None:
        raise ValueError("PNG without IHDR")
    ch = _CHANNELS[color_type]
    raw = zlib.decompress(b"".join(idat))
    stride = width * ch
    if len(raw) < (stride + 1) * height:
        raise ValueError("truncated image data")
    out = bytearray(stride * height)
    prev = bytes(stride)
    i = 0
    for y in range(height):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        if f == 1:      # Sub
            for x in range(ch, stride):
                line[x] = (line[x] + line[x - ch]) & 255
        elif f == 2:    # Up
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif f == 3:    # Average
            for x in range(stride):
                left = line[x - ch] if x >= ch else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 255
        elif f == 4:    # Paeth
            for x in range(stride):
                a = line[x - ch] if x >= ch else 0
                b = prev[x]
                c = prev[x - ch] if x >= ch else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        elif f != 0:
            raise ValueError(f"unknown filter {f} on row {y}")
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, _to_rgba(out, ch, width * height)


def _to_rgba(pixels: bytearray, ch: int, count: int) -> bytearray:
    if ch == 4:
        return pixels
    rgba = bytearray(count * 4)
    if ch == 3:
        rgba[0::4], rgba[1::4], rgba[2::4] = pixels[0::3], pixels[1::3], pixels[2::3]
        rgba[3::4] = b"\xff" * count
    elif ch == 2:
        grey = pixels[0::2]
        rgba[0::4], rgba[1::4], rgba[2::4], rgba[3::4] = grey, grey, grey, pixels[1::2]
    else:
        rgba[0::4], rgba[1::4], rgba[2::4] = pixels, pixels, pixels
        rgba[3::4] = b"\xff" * count
    return rgba


def _chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)


def encode(img_or_width, height: int | None = None, rgba: bytes | None = None) -> bytes:
    """An RGBA PNG of an image (or of width, height, rgba)."""
    if height is None:
        width, height, rgba = img_or_width
    else:
        width = img_or_width
    stride = width * 4
    if len(rgba) != stride * height:
        raise ValueError(f"{len(rgba)} bytes for a {width}x{height} RGBA image")
    raw = b"".join(b"\x00" + bytes(rgba[y * stride:(y + 1) * stride]) for y in range(height))
    return (PNG_SIGNATURE + _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            _chunk(b"IDAT", zlib.compress(raw, 6)) + _chunk(b"IEND", b""))


def crop(img, x: int, y: int, w: int, h: int):
    """The w x h rectangle at (x, y), clamped to the image."""
    width, height, rgba = img
    x0, y0 = max(0, int(x)), max(0, int(y))
    x1, y1 = min(width, int(x) + int(w)), min(height, int(y) + int(h))
    cw, chh = max(0, x1 - x0), max(0, y1 - y0)
    out = bytearray(cw * chh * 4)
    for row in range(chh):
        src = ((y0 + row) * width + x0) * 4
        out[row * cw * 4:(row + 1) * cw * 4] = rgba[src:src + cw * 4]
    return cw, chh, out


def same_frame(a, b):
    """Two shots of one camera at different view sizes, cut to what both show at one size: the
    same height (a perspective camera's vertical field of view, an orthographic camera's size, spans
    the view's height), then the middle columns both have. The Scene view's size follows the
    editor's layout (a toolbar line more is a few pixels less), its camera does not."""
    height = min(a[1], b[1])

    def to_height(img):
        if img[1] == height:
            return img
        return downscale(img, max(1, round(img[0] * height / img[1])), height)

    a, b = to_height(a), to_height(b)
    width = min(a[0], b[0])

    def middle(img):
        return img if img[0] == width else crop(img, (img[0] - width) // 2, 0, width, height)

    return middle(a), middle(b)


def downscale(img, w: int, h: int):
    """A box-filtered w x h copy: every output pixel is the area-weighted mean of the source
    pixels it covers (exact for integer ratios, area weights otherwise)."""
    width, height, rgba = img
    if (w, h) == (width, height):
        return width, height, bytearray(rgba)
    if w <= 0 or h <= 0 or w > width or h > height:
        raise ValueError(f"cannot downscale {width}x{height} to {w}x{h}")
    if width % w == 0 and height % h == 0:
        return _downscale_integer(img, width // w, height // h)
    # area weights per axis: (source index, weight) lists
    def spans(src: int, dst: int):
        scale = src / dst
        result = []
        for d in range(dst):
            a, b = d * scale, (d + 1) * scale
            parts, s = [], int(a)
            while s < b and s < src:
                wgt = min(b, s + 1) - max(a, s)
                if wgt > 0:
                    parts.append((s, wgt))
                s += 1
            result.append(parts)
        return result
    xs, ys = spans(width, w), spans(height, h)
    out = bytearray(w * h * 4)
    for oy, yparts in enumerate(ys):
        for ox, xparts in enumerate(xs):
            acc = [0.0, 0.0, 0.0, 0.0]
            total = 0.0
            for sy, wy in yparts:
                base = sy * width
                for sx, wx in xparts:
                    wgt = wx * wy
                    p = (base + sx) * 4
                    acc[0] += rgba[p] * wgt
                    acc[1] += rgba[p + 1] * wgt
                    acc[2] += rgba[p + 2] * wgt
                    acc[3] += rgba[p + 3] * wgt
                    total += wgt
            o = (oy * w + ox) * 4
            for c in range(4):
                out[o + c] = min(255, int(acc[c] / total + 0.5))
    return w, h, out


def _downscale_integer(img, fx: int, fy: int):
    width, height, rgba = img
    w, h = width // fx, height // fy
    n = fx * fy
    half = n // 2
    out = bytearray(w * h * 4)
    stride = width * 4
    for oy in range(h):
        rows = [rgba[(oy * fy + dy) * stride:(oy * fy + dy + 1) * stride] for dy in range(fy)]
        for c in range(4):
            # per channel: sum the fx samples of each row, then the fy rows
            sums = [0] * w
            for row in rows:
                channel = row[c::4]
                for dx in range(fx):
                    samples = channel[dx::fx]
                    sums = [s + v for s, v in zip(sums, samples)]
            out[oy * w * 4 + c:(oy + 1) * w * 4:4] = bytes((s + half) // n for s in sums)
    return w, h, out


def premultiply(img):
    """The image with each colour channel as round(c * a / 255): colour under alpha 0 is 0, the
    form editor/util/ImageDiff.cpp compares (diff_images, check_title's golden)."""
    width, height, rgba = img
    out = bytearray(rgba)
    for i in range(0, len(out), 4):
        a = out[i + 3]
        for c in range(3):
            # round half away from zero, as std::lround
            out[i + c] = int(out[i + c] * a / 255.0 + 0.5)
    return width, height, out


def stats(a, b, premultiplied: bool = True, tolerance: int = 8):
    """The diff_images metrics: {max, mean, fraction_over, changed_pct} with the maths of
    editor/util/ImageDiff.cpp (None when the sizes differ)."""
    if (a[0], a[1]) != (b[0], b[1]):
        return None
    if premultiplied:
        a, b = premultiply(a), premultiply(b)
    ar, br = a[2], b[2]
    count = a[0] * a[1]
    total = over = changed = biggest = 0
    for i in range(0, count * 4, 4):
        d = 0
        for c in range(4):
            dc = abs(ar[i + c] - br[i + c])
            total += dc
            d = max(d, dc)
        biggest = max(biggest, d)
        changed += d > 0
        over += d > tolerance
    pixels = max(count, 1)
    return {"max": biggest, "mean": total / (pixels * 4.0), "fraction_over": over / pixels,
            "changed_pct": 100.0 * changed / pixels}


def centre_match(outer: tuple, inner: tuple, channel_tolerance: int, search: int = 2) -> tuple:
    """A trimmed image inside a larger capture, about its centre: `inner` (composited over black,
    straight alpha) searched at up to `search` pixels of offset from where it would sit centred in
    `outer` (a title's frame rounds by a pixel). Returns (fraction of pixels over the tolerance,
    (x, y) of the best offset, worst channel difference there)."""
    (ow, oh, orgba), (iw, ih, irgba) = outer, inner
    best = (1.1, (0, 0), 0)
    centre_x, centre_y = (ow - iw) // 2, (oh - ih) // 2
    for dy in range(-search, search + 1):
        for dx in range(-search, search + 1):
            ox, oy = centre_x + dx, centre_y + dy
            if ox < 0 or oy < 0 or ox + iw > ow or oy + ih > oh:
                continue
            over = worst = 0
            for y in range(ih):
                for x in range(iw):
                    oi = ((y + oy) * ow + x + ox) * 4
                    ii = (y * iw + x) * 4
                    alpha = irgba[ii + 3]
                    for c in range(3):
                        diff = abs(orgba[oi + c] - irgba[ii + c] * alpha // 255)
                        worst = max(worst, diff)
                        if diff > channel_tolerance:
                            over += 1
                            break
            fraction = over / max(1, iw * ih)
            if fraction < best[0]:
                best = (fraction, (ox, oy), worst)
    return best


def compare(a, b, channel_tolerance: int = 24, max_fraction: float = 0.01, premultiplied: bool = False):
    """(ok, fraction, diff_png): a pixel differs when any channel (alpha included) differs by more
    than channel_tolerance; ok when at most max_fraction of the pixels differ. diff_png shows a
    dimmed grey copy of `a` with the differing pixels in red (None when the sizes differ).
    premultiplied: colours are compared as c * a / 255 (colour under alpha 0 never counts)."""
    aw, ah, ar = a
    bw, bh, br = b
    if (aw, ah) != (bw, bh):
        return False, 1.0, None
    if premultiplied:
        ar, br = premultiply(a)[2], premultiply(b)[2]
    count = aw * ah
    if count == 0:
        return True, 0.0, encode(aw, ah, bytearray())
    tol = channel_tolerance
    differing = 0
    diff = bytearray(count * 4)
    for i in range(0, count * 4, 4):
        if (abs(ar[i] - br[i]) > tol or abs(ar[i + 1] - br[i + 1]) > tol or
                abs(ar[i + 2] - br[i + 2]) > tol or abs(ar[i + 3] - br[i + 3]) > tol):
            differing += 1
            diff[i], diff[i + 1], diff[i + 2], diff[i + 3] = 255, 0, 0, 255
        else:
            grey = (ar[i] * 30 + ar[i + 1] * 59 + ar[i + 2] * 11) // 300
            diff[i] = diff[i + 1] = diff[i + 2] = grey
            diff[i + 3] = 255
    fraction = differing / count
    return fraction <= max_fraction, fraction, encode(aw, ah, diff)


def flat(img, tolerance: int = 2) -> bool:
    """True when every pixel equals the first one within `tolerance` per channel (an empty or
    blank capture)."""
    width, height, rgba = img
    if width * height == 0:
        return True
    for c in range(4):
        channel = rgba[c::4]
        if max(channel) - min(channel) > tolerance:
            return False
    return True


def load(path) -> tuple:
    with open(path, "rb") as f:
        return decode(f.read())


def main(argv: list[str]) -> int:
    import argparse

    parser = argparse.ArgumentParser(description="Compare two PNG images (tools/mcp/pngdiff.py).")
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--size", help="downscale both to WxH first (e.g. 640x360)")
    parser.add_argument("--tolerance", type=int, default=24)
    parser.add_argument("--max-fraction", type=float, default=0.01)
    parser.add_argument("--diff", help="write the diff image here")
    parser.add_argument("--premultiplied", action="store_true",
                        help="compare premultiplied colour (c * a / 255), as diff_images does")
    args = parser.parse_args(argv)
    a, b = load(args.a), load(args.b)
    if args.size:
        w, h = (int(v) for v in args.size.lower().split("x"))
        a, b = downscale(a, w, h), downscale(b, w, h)
    ok, fraction, diff_png = compare(a, b, args.tolerance, args.max_fraction, args.premultiplied)
    print(f"{'match' if ok else 'DIFFERENT'}: {fraction * 100:.3f}% of the pixels differ "
          f"(tolerance {args.tolerance}, at most {args.max_fraction * 100:.2f}%)")
    if args.diff and diff_png:
        with open(args.diff, "wb") as f:
            f.write(diff_png)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
