"""The high-resolution regression fixtures of SpriteNormalsHiresTest.cpp (README.md here).

    python3 -I make_hires_fixtures.py <real sprites dir> <editor before high-resolution support>

Needs numpy, scipy and Pillow. Writes, beside this script:
- synthetic ground truth (analytic height fields shaded Lambert from the upper left, 4x4 supersampled silhouettes):
  sphere_256.png, torus_256.png, roundbox_256.png (transparent around) and cobbles_256.png (a seamless opaque tile),
  each with <name>_true.png: the exact normals (OpenGL, green up) encoded (128, 128, 255)-style, alpha 255 where the
  pixel centre is inside the shape (the pixels the error is measured on);
- four CC0 sprites from the evaluation set downscaled (Lanczos on premultiplied colour) so the longer side is 384:
  crate.png, potion.png, lavapotion.png, barbarian.png (sources and licences in README.md);
- v1/<name>_n.png: what the editor before high-resolution support made of sphere_256 and potion with the default
  preset (`generate-sprite-normals --preset default`): a version 1 recipe must regenerate these bytes exactly.
"""
import os, subprocess, sys
import numpy as np
from PIL import Image
from scipy import ndimage, spatial

HERE = os.path.dirname(os.path.abspath(__file__))
LIGHT = np.array([-0.45, 0.55, 0.70]); LIGHT /= np.linalg.norm(LIGHT)


def encode(n, alpha):
    return np.dstack([np.round((n * 0.5 + 0.5) * 255), alpha]).astype(np.uint8)


def shape(kind, S):
    c = (S - 1) / 2.0
    def f(x, y):
        X, Y = x - c, y - c
        if kind == 'sphere':
            r = 0.44 * S; inside = X * X + Y * Y < r * r; h = np.sqrt(np.maximum(r * r - X * X - Y * Y, 1e-9))
            return inside, -X / h, -Y / h
        if kind == 'torus':
            R, a = 0.29 * S, 0.14 * S; rho = np.sqrt(X * X + Y * Y) + 1e-9; q = rho - R
            inside = np.abs(q) < a; h = np.sqrt(np.maximum(a * a - q * q, 1e-9))
            return inside, -q / h * X / rho, -q / h * Y / rho
        hw, hh, b = 0.42 * S, 0.30 * S, 0.10 * S   # roundbox: flat top, quarter-round rim
        qx = np.maximum(np.abs(X) - (hw - b), 0) * np.sign(X); qy = np.maximum(np.abs(Y) - (hh - b), 0) * np.sign(Y)
        d2 = qx * qx + qy * qy; inside = (np.abs(X) < hw) & (np.abs(Y) < hh) & (d2 < b * b)
        h = np.sqrt(np.maximum(b * b - d2, 1e-9))
        return inside, -qx / h, -qy / h
    return f


def normals(gx, gy):
    n = np.stack([-gx, gy, np.ones_like(gx)], -1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


def render(name, kind, S, base, ss=4):
    f = shape(kind, S)
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float64)
    acc = np.zeros((S, S, 3)); cov = np.zeros((S, S))
    for sy in range(ss):
        for sx in range(ss):
            inside, gx, gy = f(xx + (sx + 0.5) / ss - 0.5, yy + (sy + 0.5) / ss - 0.5)
            lam = np.clip(normals(gx, gy) @ LIGHT, 0, 1)
            acc += base[None, None, :] * (0.28 + 0.78 * lam[..., None]) * inside[..., None]; cov += inside
    alpha = cov / (ss * ss)
    col = np.clip(acc / np.maximum(cov, 1e-9)[..., None], 0, 1)
    rgba = np.dstack([col, alpha]); rgba[alpha == 0] = 0
    Image.fromarray(np.round(rgba * 255).astype(np.uint8), 'RGBA').save(os.path.join(HERE, name + '.png'))
    inside, gx, gy = f(xx, yy)
    n = normals(gx, gy); n[~inside] = (0, 0, 1)
    Image.fromarray(encode(n, np.where(inside, 255, 0)), 'RGBA').save(os.path.join(HERE, name + '_true.png'))


def cobbles(name, S, cells=48, seed=3):
    rng = np.random.default_rng(seed)
    pts = rng.random((cells, 2)) * S
    tree = spatial.cKDTree(np.concatenate([pts + np.array([dx, dy]) * S for dx in (-1, 0, 1) for dy in (-1, 0, 1)]))
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float64)
    d, i = tree.query(np.stack([xx.ravel() + 0.5, yy.ravel() + 0.5], -1), k=2)
    e = ((d[:, 1] - d[:, 0]) / 2).reshape(S, S); cell = (i[:, 0] % cells).reshape(S, S)
    w = 0.035 * S; mort = 0.006 * S
    t = np.clip((e - mort) / w, 0, 1)
    h = ndimage.gaussian_filter(w * 0.55 * np.sqrt(np.clip(1 - (1 - t) ** 2, 0, 1)), 0.6, mode='wrap')
    gy, gx = np.gradient(h)
    n = normals(gx, gy)
    tone = 0.45 + 0.25 * rng.random(cells)
    v = np.where(e > mort, tone[cell], 0.18) * (0.35 + 0.75 * np.clip(n @ LIGHT, 0, 1))
    col = np.clip(np.stack([v, v * 0.95, v * 0.88], -1), 0, 1)
    Image.fromarray(np.dstack([np.round(col * 255), np.full((S, S), 255)]).astype(np.uint8), 'RGBA').save(os.path.join(HERE, name + '.png'))
    Image.fromarray(encode(n, np.full((S, S), 255)), 'RGBA').save(os.path.join(HERE, name + '_true.png'))


def downscale(src, dst, longest=384):
    im = Image.open(src).convert('RGBA')
    k = longest / max(im.size)
    a = np.asarray(im).astype(np.float64) / 255.0
    pre = np.dstack([a[..., :3] * a[..., 3:4], a[..., 3]])
    size = (max(1, round(im.width * k)), max(1, round(im.height * k)))
    ch = [np.asarray(Image.fromarray(pre[..., c].astype(np.float32), 'F').resize(size, Image.LANCZOS)) for c in range(4)]
    alpha = np.clip(ch[3], 0, 1)
    rgb = np.clip(np.stack(ch[:3], -1) / np.maximum(alpha, 1e-6)[..., None], 0, 1)
    out = np.dstack([np.round(rgb * 255), np.round(alpha * 255)]).astype(np.uint8)
    out[out[..., 3] == 0] = 0
    Image.fromarray(out, 'RGBA').save(os.path.join(HERE, dst))


if __name__ == '__main__':
    real, old_editor = sys.argv[1], sys.argv[2]
    render('sphere_256', 'sphere', 256, np.array([0.80, 0.22, 0.18]))
    render('torus_256', 'torus', 256, np.array([0.85, 0.66, 0.25]))
    render('roundbox_256', 'roundbox', 256, np.array([0.30, 0.42, 0.82]))
    cobbles('cobbles_256', 256)
    for src, dst in [('crate_drawn.png', 'crate.png'), ('healthpotion_painted.png', 'potion.png'),
                     ('lavapotion_glossy.png', 'lavapotion.png'), ('barbarian_flat.png', 'barbarian.png')]:
        downscale(os.path.join(real, src), dst)
    os.makedirs(os.path.join(HERE, 'v1'), exist_ok=True)
    for name in ['sphere_256', 'potion']:
        subprocess.run([old_editor, 'generate-sprite-normals', '-s', os.path.join(HERE, name + '.png'), '-o',
                        os.path.join(HERE, 'v1', name + '_n.png'), '--preset', 'default', '--overwrite', '-q'], check=True)
