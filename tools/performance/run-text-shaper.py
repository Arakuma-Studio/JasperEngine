#!/usr/bin/env python3
"""Compare exact production text shapers, with real bundled fonts/HarfBuzz/SheenBidi.

Uses one process and alternating order for paired CPU measurements. These are
uncached text-shaping costs, not UI frame times. The comparison checks every
glyph field, including advances, clusters, fallback faces and visual order.
"""
import argparse
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", default="HEAD")
    parser.add_argument("--output", type=Path, default=ROOT / "build/performance-text-shaper")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for name in ("TextShaper.h", "TextShaper.cpp"):
        source = subprocess.check_output(
            ["git", "show", f"{args.baseline_ref}:engine/core/util/{name}"], cwd=ROOT, text=True)
        source = source.replace("TEXTSHAPER_H", "BASELINETEXTSHAPER_H")
        source = source.replace("TextShaper", "BaselineTextShaper")
        # ShapeOptions and ShapedGlyph are the same public structs in both revisions.
        if name.endswith(".h"):
            begin = source.index("    // a glyph the shaper picked")
            end = source.index("    // HarfBuzz + SheenBidi shaping")
            source = source[:begin] + source[end:]
            source = '#include "TextShaper.h"\n' + source
        (out / name.replace("TextShaper", "BaselineTextShaper")).write_text(source)
    root = ROOT.as_posix()
    out.joinpath("CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(text-shaper-perf LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
add_subdirectory("{root}/engine/libs/brotli" brotli)
add_subdirectory("{root}/engine/libs/woff2" woff2)
add_subdirectory("{root}/engine/libs/harfbuzz" harfbuzz)
add_subdirectory("{root}/engine/libs/sheenbidi" sheenbidi)
add_executable(text-shaper
    "{root}/tools/performance/text_shaper.cpp"
    "{root}/engine/core/util/TextShaper.cpp"
    "{out.as_posix()}/BaselineTextShaper.cpp"
    "{root}/engine/core/util/FontFace.cpp"
    "{root}/engine/core/util/StringUtils.cpp"
    "{root}/engine/libs/stb/stb_truetype.c"
    "{root}/engine/libs/stb/stb_image.c")
target_include_directories(text-shaper PRIVATE "{out.as_posix()}"
    "{root}/engine/core" "{root}/engine/core/util" "{root}/engine/libs/stb")
target_compile_options(text-shaper PRIVATE -ffunction-sections -fdata-sections)
target_link_options(text-shaper PRIVATE -Wl,--gc-sections)
target_link_libraries(text-shaper PRIVATE harfbuzz sheenbidi woff2)
if(TEXT_SHAPER_SANITIZE)
    target_compile_definitions(text-shaper PRIVATE TEXT_SHAPER_CHECK_ONLY)
    # The unused loadFile method is discarded. Its UBSan vptr metadata would
    # otherwise retain Data's RTTI even though this fixture loads fonts in memory.
    set_source_files_properties("{root}/engine/core/util/FontFace.cpp"
        PROPERTIES COMPILE_OPTIONS "-fno-sanitize=vptr")
    foreach(target text-shaper harfbuzz sheenbidi woff2 brotlidec)
        target_compile_options(${{target}} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${{target}} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()
''')
    binary_dir = out / ("sanitize" if args.sanitize else "release")
    subprocess.run(["cmake", "-S", str(out), "-B", str(binary_dir), "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=" + ("Debug" if args.sanitize else "Release"),
                    "-DTEXT_SHAPER_SANITIZE=" + ("ON" if args.sanitize else "OFF")], check=True)
    subprocess.run(["cmake", "--build", str(binary_dir), "--parallel", "1"], check=True)
    output = subprocess.check_output([str(binary_dir / "text-shaper")], text=True, cwd=ROOT)
    print(output, end="")
    (out / ("checks.txt" if args.sanitize else "samples.csv")).write_text(output)
    if not args.sanitize:
        rows = [line.split(",") for line in output.splitlines() if line.count(",") == 4]
        for case in dict.fromkeys(row[0] for row in rows):
            samples = [row for row in rows if row[0] == case]
            before = statistics.median(float(row[2]) for row in samples)
            after = statistics.median(float(row[3]) for row in samples)
            ratio = statistics.median(float(row[4]) for row in samples)
            print(f"{case}: {before:.1f} -> {after:.1f} CPU ns/call; median paired ratio {ratio:.4f}")


if __name__ == "__main__":
    main()
