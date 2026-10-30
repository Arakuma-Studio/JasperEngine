#!/usr/bin/env python3
"""What takes the space in a native library (docs/specs/S30-shipping.md, shipped size): the
symbols of a library (a player runtime's symbols/, or the build's own output) grouped by what
they belong to. Code and data symbols count (nm types t/T, d/D, b/B, r/R, w/W, v/V, u).

By name (the default, fast): the third-party libraries of engine/libs by their prefixes and
namespaces (glslang::, JPH::, Rml::, dav1d_...), the engine's areas by namespace and class name
(engine: fluid, engine: navigation...), the C++ standard library, the Android glue.
By line (--lines, slow on a large library): the source directory each symbol's debug information
names (engine/libs/<library>, engine/core/<area>...).

    python3 tools/players/lib_size.py <library.so> [--lines] [--nm NM] [--top 40] [--json out.json]
    python3 tools/players/lib_size.py --from-nm <nm-output.txt> [--lines] ...   (the output of
        nm --print-size --size-sort --demangle [--line-numbers] <library>)

NM defaults to llvm-nm, then nm. Standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

LINE = re.compile(r"^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([A-Za-z])\s+(.*?)(?:\t(\S+?):\d+)?$")
KINDS = set("tTdDbBrRwWvVu")


def group(path: str | None) -> str:
    """The group a source path belongs to."""
    if not path:
        return "(no line information)"
    p = path.replace("\\", "/")
    if "/c++/v1/" in p or "/include/c++/" in p or "/libcxx/" in p:
        return "C++ standard library (templates)"
    if "/sysroot/" in p or p.startswith("/usr/include/"):
        return "system headers"
    for marker, depth in (("engine/libs/", 1), ("engine/core/", 1), ("engine/renders/", 1), ("engine/platform/", 1),
                          ("engine/player/", 0), ("engine/project/", 0), ("/project/", 0), ("editor/", 0)):
        at = p.find(marker)
        if at >= 0:
            rest = p[at + len(marker):].split("/")
            if depth == 0 or len(rest) <= 1:
                return marker.strip("/") if depth == 0 else marker + "(top level)"
            # the libraries a library vendors (shadercompiler/libs/glslang): their own group
            if len(rest) > 3 and rest[1] == "libs" and marker == "engine/libs/":
                return marker + rest[0] + "/" + rest[2]
            return marker + rest[0]
    if "/.cxx/" in p or "/build/" in p:
        return "generated (build tree)"
    return "other: " + "/".join(p.split("/")[-3:-1])


# (pattern on the demangled name, group): the first that matches names the symbol's group
NAMES = [(re.compile(p), g) for p, g in [
    (r"^(glslang::|TPpContext|spv::|QualifierFlags|ShInitialize|ShFinalize|yy[a-z]+\b|TGlslangToSpv)", "lib: glslang (runtime shader compiler)"),
    (r"^shadercompiler", "lib: shadercompiler (wrapper)"),
    (r"^spirv_cross::|^spvc_", "lib: spirv-cross (runtime shader compiler)"),
    (r"^JPH::", "lib: joltphysics (3D physics)"),
    (r"^b2[A-Z_]", "lib: box2d (2D physics)"),
    (r"^(dt|rc|du)[A-Z]", "lib: recastnavigation (navigation)"),
    (r"^Rml::", "lib: rmlui (HTML UI)"),
    (r"^tvg::|^tvg[A-Z_]|^Lottie|^fillRaster|^_rasterize", "lib: thorvg (HTML UI vector, Lottie)"),
    (r"^lunasvg::|^plutovg_", "lib: lunasvg (HTML UI SVG)"),
    (r"^(lxb_|lexbor_)", "lib: lexbor (HTML UI parser)"),
    (r"^(dav1d|qm_tbl|default_.*_cdf|decode_b$|decode_coefs|dav1d_)", "lib: dav1d (video AV1)"),
    (r"^(opus|silk_|celt_|ec_|quant_|clt_|alg_|spreading_|tf_select)", "lib: opus (video audio)"),
    (r"^(mkvparser|mkvmuxer)::", "lib: libwebm (video container)"),
    (r"^(hb_|OT::|AAT::|CFF::|_hb_|graph::|find_syllables)", "lib: harfbuzz (text shaping)"),
    (r"^SB[A-Z]|^Primary\w+Data$", "lib: sheenbidi (bidi text)"),
    (r"^(lua[A-Z_]|luaopen_|luai_)", "lib: lua"),
    (r"^luabridge::", "lib: luabridge3 (Lua bindings, templates)"),
    (r"^(ZSTD|HUF_|FSE_|ZSTDv)", "lib: zstd"),
    (r"^(basist|basisu)::", "lib: basisu (texture transcoder)"),
    (r"^msdfgen::|^msdf_", "lib: msdfgen (font atlases)"),
    (r"^tinygltf::", "lib: tinygltf"),
    (r"^tinyobj::", "lib: tinyobjloader"),
    (r"^tinyxml2::", "lib: tinyxml2"),
    (r"^(SoLoud::|soloud_|Soloud_|ma_|drwav_|drmp3|drflac_|stb_vorbis|fftimpl)", "lib: soloud + miniaudio (audio)"),
    (r"^robin_hood::", "lib: robin_hood (hash maps)"),
    (r"^jpeg_decoder|^jpgd::", "lib: jpgd (JPEG)"),
    (r"^(stbi|stbtt|stbir|stb_)", "lib: stb"),
    (r"^meshopt_", "lib: meshoptimizer"),
    (r"^nlohmann::|^_ZN\w*8nlohmann", "lib: nlohmann json"),
    (r"^(sg_|_sg_|_sg|sgj_|sapp_|_sapp|sgl_|sdtx_|smemtrack)", "lib: sokol (graphics backend)"),
    (r"^bcdec_", "lib: bcdec"),
    (r"^(GameActivity|Swappy|swappy::|gamesdk::|android_app|Java_com_google|_GameActivity|ndk_helper)", "android: GameActivity + Swappy"),
    (r"^(std::|__cxx|__gnu|__cxa|__gxx|operator new|operator delete|__libcpp|__ndk1|libunwind|_Unwind|_ZN\w*St6__ndk1|_ZNSt|_ZNKSt)",
     "C++ standard library"),
    (r"_font_ttf$|_ttf$|_otf$", "engine: embedded fonts"),
    (r"^(je_crash|je_hang|je_logger|je_lua|je_)", "engine: diagnostics and logging"),
    (r"^NativeEngine|^JasperEngineAndroid|^android_main", "engine: Android platform"),
    (r"^_ZN\w*12jasperengine", "engine: other"),
]]
ENGINE_AREAS = [(re.compile(p, re.I), g) for p, g in [
    (r"htmlui|HtmlUI|UIDocument|Rml", "engine: HTML UI"),
    (r"fluid", "engine: fluid simulation"),
    (r"nav(igation|mesh|grid|agent)?|crowd", "engine: navigation"),
    (r"spline", "engine: splines"),
    (r"lattice", "engine: lattice deformer"),
    (r"particle", "engine: particles"),
    (r"terrain|foliage", "engine: terrain"),
    (r"physics|body|joint|collider|character|buoyan|raycast", "engine: physics"),
    (r"video|webm", "engine: video"),
    (r"audio|sound", "engine: audio"),
    (r"lua|script", "engine: Lua and scripts"),
    (r"shader|sokol|render|texture|framebuffer|material|light|shadow|camera|sky|postprocess|uber|gpu|draw", "engine: rendering"),
    (r"mesh|model|gltf|obj|skin|bone|morph|pose|anim|action|tween|keyframe|clip", "engine: meshes and animation"),
    (r"font|text|glyph|label|ui|button|panel|image|container|scrollbar|progress|input", "engine: UI (sprites, text)"),
    (r"scene|entity|registry|system|component|transform|engine|ecs", "engine: scene and ECS"),
    (r"cook|jpak|serializ|pack|resource|file|content|player|launch", "engine: content and player"),
]]


def head_of(name: str) -> str:
    """The symbol's qualified name without what demangling puts before it: a function template's
    return type ("void jasperengine::f<int>(...)") and "(anonymous namespace)::"."""
    n = name
    if n.startswith(("vtable for", "typeinfo", "guard variable", "non-virtual thunk", "virtual thunk", "construction vtable")):
        return n
    end = n.find("(")
    end = len(n) if end < 0 else end
    depth, cut = 0, -1
    for i, ch in enumerate(n[:end]):
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        elif ch == " " and depth == 0 and not n[:i].endswith("operator"):
            cut = i
    if cut >= 0:
        n = n[cut + 1:]
    while n.startswith("(anonymous namespace)::"):
        n = n[len("(anonymous namespace)::"):]
    return n.lstrip("*&")


def name_group(name: str) -> str:
    """The group of a demangled symbol name."""
    name = head_of(name)
    for pattern, g in NAMES:
        if pattern.search(name):
            return g
    if name.startswith("jasperengine::"):
        rest = name[len("jasperengine::"):]
        head = re.split(r"::|\(|<| ", rest, maxsplit=1)[0]
        for pattern, g in ENGINE_AREAS:
            if pattern.search(head):
                return g
        return "engine: other"
    if name.startswith(("vtable for jasperengine", "typeinfo for jasperengine", "typeinfo name for jasperengine",
                        "guard variable for jasperengine")):
        return "engine: vtables and RTTI"
    if name.startswith(("vtable for", "typeinfo")):
        return "vtables and RTTI (other)"
    return "other (file-local helpers, unattributed)"


def parse(lines, by_lines: bool = False) -> dict[str, list[int]]:
    """{group: [bytes, symbols]}"""
    groups: dict[str, list[int]] = defaultdict(lambda: [0, 0])
    for line in lines:
        m = LINE.match(line.rstrip("\n"))
        if not m or m.group(3) not in KINDS:
            continue
        size = int(m.group(2), 16)
        g = groups[group(m.group(5)) if by_lines else name_group(m.group(4))]
        g[0] += size
        g[1] += 1
    return dict(groups)


def run_nm(library: Path, nm: str | None, by_lines: bool) -> list[str]:
    tool = nm or shutil.which("llvm-nm") or shutil.which("nm")
    if not tool:
        sys.exit("no nm: pass --nm")
    out = subprocess.run([tool, "--print-size", "--size-sort", "--demangle", *(["--line-numbers"] if by_lines else []), str(library)],
                         capture_output=True, text=True, errors="replace")
    if out.returncode != 0:
        sys.exit(f"{tool} failed: {out.stderr[-2000:]}")
    return out.stdout.splitlines()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("library", nargs="?", type=Path)
    ap.add_argument("--from-nm", type=Path, help="read nm's output instead of running it")
    ap.add_argument("--nm", help="the nm to run (default llvm-nm, then nm)")
    ap.add_argument("--lines", action="store_true", help="group by the source directory of the debug information (slow)")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    if args.from_nm:
        lines = args.from_nm.read_text(errors="replace").splitlines()
    elif args.library:
        lines = run_nm(args.library, args.nm, args.lines)
    else:
        ap.error("a library or --from-nm")
    groups = parse(lines, args.lines)
    total = sum(v[0] for v in groups.values()) or 1
    ranked = sorted(groups.items(), key=lambda kv: -kv[1][0])
    print(f"{'bytes':>12} {'share':>6} {'symbols':>8}  group")
    for name, (size, count) in ranked[:args.top]:
        print(f"{size:>12,} {100.0 * size / total:>5.1f}% {count:>8,}  {name}")
    rest = ranked[args.top:]
    if rest:
        print(f"{sum(v[0] for _, v in rest):>12,} {100.0 * sum(v[0] for _, v in rest) / total:>5.1f}% "
              f"{sum(v[1] for _, v in rest):>8,}  ({len(rest)} more groups)")
    print(f"{total:>12,} 100.0% {sum(v[1] for v in groups.values()):>8,}  total (symbol sizes)")
    if args.json:
        args.json.write_text(json.dumps({name: {"bytes": v[0], "symbols": v[1]} for name, v in ranked}, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
