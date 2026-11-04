#!/usr/bin/env python3
"""Packages a built jasperengine-player into a player runtime (docs/specs/S30-shipping.md R8, D4).

    python3 tools/players/package_player.py --platform linux --arch x64 --backend glcore \\
        --flavour release [--variant lite3d] --build build/players-build/linux-x64-glcore-release --out build/players [--zip DIR]

A runtime is a directory named by its id (<platform>-<arch>-<backend>-<flavour>[-<variant>]: a
release variant of engine/player/variants.json, docs/specs/S34-player-size.md R4; the full player
has the plain id):

    player-info.json          schema 1: id, engineVersion, gitSha, contentFormatVersion, modelCookFormats, capacities,
                              features (the variant's modules), variant, notices,
                              executables {path: sha256}, executable
    files/                    what a build stages as it is (the executable; the web's two players
                              and page; a macOS bundle)
    android/ | ios/           the platform template (R13, R14)
    licenses/                 <lib>-LICENSE of every library the player links (engine/notices.json)
    symbols/                  the player's debug information (--symbols: the .debug of a stripped
                              Linux player, Android's unstripped libraries); never staged into a build,
                              archived on its own as jasperengine-player-<id>-<version>-symbols.zip
    THIRD-PARTY-NOTICES.txt   their NOTICE.md rows

--zip DIR also writes jasperengine-player-<id>-<engineVersion>.zip there (what `players install`
takes). Platform inputs:
  linux / windows   --build: the CMake tree of the player (jasperengine-player[.exe], its .dll files)
  macos             --build: the tree with jasperengine-player.app
  web               --build: the single-thread tree, --build-mt: the pthreads one (optional)
  android           --android-dir: what tools/players/android_player.sh collected (res-flat.zip,
                    stable-ids.txt, classes*.dex, lib/<abi>/*.so)
  ios               --ios-dir: JasperEnginePlayer.xcframework and template/ (the Xcode project)
Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WEB_TEMPLATE = REPO / "engine/platform/emscripten/web"
ANDROID_TEMPLATE = REPO / "engine/platform/android/player"
CAPACITIES = ("MAX_SUBMESHES", "MAX_TILEMAP_TILESRECT", "MAX_TILEMAP_TILES", "MAX_SPRITE_FRAMES", "MAX_EXTERNAL_BUFFERS",
              "MAX_BONES")
VARIANTS = REPO / "engine/player/variants.json"


def variant_of(name: str) -> dict:
    """A variant of engine/player/variants.json: {name, suffix, modules}."""
    variants = json.loads(VARIANTS.read_text(encoding="utf-8"))["variants"]
    for variant in variants:
        if variant["name"] == name:
            return variant
    raise SystemExit(f"--variant {name} is not in {VARIANTS} ({', '.join(v['name'] for v in variants)})")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def capacities() -> dict[str, int]:
    text = (REPO / "engine/core/Engine.h").read_text(encoding="utf-8")
    found = {}
    for name in CAPACITIES:
        match = re.search(rf"^#define {name} (\d+)", text, re.MULTILINE)
        if not match:
            raise SystemExit(f"engine/core/Engine.h has no #define {name}")
        found[name] = int(match.group(1))
    return found


def content_format_version() -> str:
    text = (REPO / "engine/core/serialization/CookedScene.h").read_text(encoding="utf-8")
    major = re.search(r"FORMAT_MAJOR = (\d+)", text)
    minor = re.search(r"FORMAT_MINOR = (\d+)", text)
    if not major or not minor:
        raise SystemExit("engine/core/serialization/CookedScene.h has no FORMAT_MAJOR/FORMAT_MINOR")
    return f"{major.group(1)}.{minor.group(1)}"


def model_cook_formats() -> list:
    """The cooked model formats the player decodes (engine/core/model/CookedModels.h): an export
    refuses a runtime without the one it writes (S53 R30)."""
    text = (REPO / "engine/core/model/CookedModels.h").read_text(encoding="utf-8")
    version = re.search(r"kFormatVersion = (\d+)", text)
    if not version:
        raise SystemExit("engine/core/model/CookedModels.h has no kFormatVersion")
    return list(range(1, int(version.group(1)) + 1))


def engine_version() -> str:
    """The version the editor of this tree reports (CMakeLists.txt's JASPERENGINE_EDITOR_VERSION):
    a runtime is locked to it (R10)."""
    try:
        described = subprocess.run(["git", "-C", str(REPO), "describe", "--tags", "--match", "v[0-9]*", "--always", "--dirty"],
                                   check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        described = ""
    return re.sub(r"[^A-Za-z0-9._+-]", "", described) or "unknown"


def git_sha() -> str:
    try:
        return subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def notices(features: list[str]) -> tuple[list[str], list[tuple[Path, str]], str]:
    """(library names, (licence file, staged name) pairs, THIRD-PARTY-NOTICES.txt) of a player."""
    sys.path.insert(0, str(REPO / "tools/ci"))
    import importlib.util
    spec = importlib.util.spec_from_file_location("check_notices", REPO / "tools/ci/check-notices.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    libraries = module.player_libraries(REPO, features)
    manifest = module.player_manifest(REPO, features)
    text = module.third_party_notices(REPO, features)
    return libraries, manifest, text


def copy_file(source: Path, target: Path, executable: bool = False) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    if executable:
        target.chmod(target.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def stage(args: argparse.Namespace, runtime: Path) -> tuple[dict[str, str], str]:
    """Copies the platform's binaries and template; returns ({path: sha256} of the binaries, main executable)."""
    files = runtime / "files"
    binaries: dict[str, str] = {}

    def record(path: Path) -> None:
        binaries[path.relative_to(runtime).as_posix()] = sha256(path)

    build = Path(args.build) if args.build else None
    if args.platform in ("linux", "windows"):
        name = "jasperengine-player.exe" if args.platform == "windows" else "jasperengine-player"
        source = build / name
        if not source.is_file():
            raise SystemExit(f"no {source}: build the player first")
        copy_file(source, files / name, executable=True)
        record(files / name)
        if args.platform == "windows":
            for dll in sorted(build.glob("*.dll")):
                copy_file(dll, files / dll.name)
                record(files / dll.name)
        return binaries, name
    if args.platform == "macos":
        bundle = build / "jasperengine-player.app"
        if not bundle.is_dir():
            raise SystemExit(f"no {bundle}: build the player first")
        target = files / "JasperEnginePlayer.app"
        shutil.copytree(bundle, target, symlinks=True)
        for path in sorted((target / "Contents/MacOS").iterdir()):
            record(path)
        return binaries, "JasperEnginePlayer.app"
    if args.platform == "web":
        for flavour, tree in (("st", build), ("mt", Path(args.build_mt) if args.build_mt else None)):
            if tree is None:
                continue
            for suffix in (".js", ".wasm", ".worker.js"):
                source = tree / f"jasperengine-player-{flavour}{suffix}"
                if source.is_file():
                    copy_file(source, files / source.name)
                    record(files / source.name)
                elif suffix != ".worker.js":
                    raise SystemExit(f"no {source}: build the {flavour} web player first")
        copy_file(WEB_TEMPLATE / "player.html", files / "index.html")
        copy_file(WEB_TEMPLATE / "player-loader.js", files / "player-loader.js")
        if args.build_mt:
            copy_file(WEB_TEMPLATE / "coi-serviceworker.js", files / "coi-serviceworker.js")
        return binaries, "index.html"
    if args.platform == "android":
        source = Path(args.android_dir)
        target = runtime / "android"
        for name in ("res-flat.zip", "stable-ids.txt"):
            if not (source / name).is_file():
                raise SystemExit(f"no {source / name}: run tools/players/android_player.sh first")
            copy_file(source / name, target / name)
        copy_file(ANDROID_TEMPLATE / "AndroidManifest.template.xml", target / "AndroidManifest.template.xml")
        dexes = sorted(source.glob("classes*.dex"))
        if not dexes:
            raise SystemExit(f"no classes*.dex in {source}")
        for dex in dexes:
            copy_file(dex, target / dex.name)
            record(target / dex.name)
        libraries = sorted((source / "lib").glob("*/*.so"))
        if not libraries:
            raise SystemExit(f"no lib/<abi>/*.so in {source}")
        for library in libraries:
            relative = library.relative_to(source)
            copy_file(library, target / relative)
            record(target / relative)
        return binaries, "android/lib"
    if args.platform == "ios":
        source = Path(args.ios_dir)
        if not (source / "JasperEnginePlayer.xcframework").is_dir() or not (source / "template").is_dir():
            raise SystemExit(f"{source} needs JasperEnginePlayer.xcframework and template/")
        shutil.copytree(source / "JasperEnginePlayer.xcframework", runtime / "ios/JasperEnginePlayer.xcframework", symlinks=True)
        shutil.copytree(source / "template", runtime / "ios/template", symlinks=True)
        for path in sorted((runtime / "ios/JasperEnginePlayer.xcframework").rglob("*")):
            if path.is_file() and not path.is_symlink() and path.suffix in ("", ".a", ".dylib"):
                record(path)
        return binaries, "ios/JasperEnginePlayer.xcframework"
    raise SystemExit(f"unknown platform {args.platform}")


def package(args: argparse.Namespace) -> Path:
    variant = variant_of(args.variant)
    if variant["suffix"] and args.flavour != "release":
        raise SystemExit(f"--variant {args.variant}: a development player is always the full one")
    runtime_id = f"{args.platform}-{args.arch}-{args.backend}-{args.flavour}" + (f"-{variant['suffix']}" if variant["suffix"] else "")
    out = Path(args.out)
    runtime = out / runtime_id
    temp = out / f".{runtime_id}.packaging"
    shutil.rmtree(temp, ignore_errors=True)
    temp.mkdir(parents=True)
    try:
        binaries, executable = stage(args, temp)
        # the modules the player was built with (the variant's); tracy only in development
        features = list(variant["modules"]) + (["tracy"] if args.flavour == "development" else [])
        libraries, manifest, third_party = notices(features)
        for source, name in manifest:
            copy_file(source, temp / "licenses" / name)
        (temp / "THIRD-PARTY-NOTICES.txt").write_text(third_party, encoding="utf-8")
        info = {
            "schemaVersion": 1,
            "id": runtime_id,
            "platform": args.platform,
            "arch": args.arch,
            "backend": args.backend,
            "flavour": args.flavour,
            "engineVersion": args.engine_version or engine_version(),
            "gitSha": git_sha(),
            "contentFormatVersion": content_format_version(),
            "modelCookFormats": model_cook_formats(),
            "capacities": capacities(),
            "features": features,
            "variant": variant["name"],
            "notices": libraries,
            "executables": binaries,
            "executable": executable,
        }
        (temp / "player-info.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
        # the debug information stays with the runtime (publish prepare copies it into a release's
        # symbols/), and never in what a build stages
        if args.symbols and Path(args.symbols).is_dir():
            shutil.copytree(args.symbols, temp / "symbols", symlinks=True)
        shutil.rmtree(runtime, ignore_errors=True)
        os.replace(temp, runtime)
    finally:
        shutil.rmtree(temp, ignore_errors=True)
    print(f"runtime: {runtime}")
    if args.zip:
        zip_dir = Path(args.zip)
        zip_dir.mkdir(parents=True, exist_ok=True)
        archive = zip_dir / f"jasperengine-player-{runtime_id}-{info['engineVersion']}.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zipped:
            for path in sorted(runtime.rglob("*")):
                if path.is_symlink() or path.is_dir() or "symbols" == path.relative_to(runtime).parts[0]:
                    continue
                entry = zipfile.ZipInfo.from_file(path, path.relative_to(runtime).as_posix())
                entry.compress_type = zipfile.ZIP_DEFLATED
                with path.open("rb") as stream:
                    zipped.writestr(entry, stream.read())
        print(f"archive: {archive} sha256 {sha256(archive)} size {archive.stat().st_size}")
        symbols = runtime / "symbols"
        if symbols.is_dir():
            symbol_archive = zip_dir / f"jasperengine-player-{runtime_id}-{info['engineVersion']}-symbols.zip"
            with zipfile.ZipFile(symbol_archive, "w", zipfile.ZIP_DEFLATED) as zipped:
                for path in sorted(symbols.rglob("*")):
                    if path.is_file() and not path.is_symlink():
                        zipped.write(path, path.relative_to(symbols).as_posix())
            print(f"symbols: {symbol_archive} size {symbol_archive.stat().st_size}")
    return runtime


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--platform", required=True, choices=["windows", "linux", "macos", "android", "ios", "web"])
    parser.add_argument("--arch", required=True)
    parser.add_argument("--backend", required=True)
    parser.add_argument("--flavour", required=True, choices=["release", "development"])
    parser.add_argument("--variant", default="full", help="the release variant the player was built for (engine/player/variants.json)")
    parser.add_argument("--build", help="the player's CMake build tree")
    parser.add_argument("--build-mt", help="web: the pthreads player's build tree")
    parser.add_argument("--android-dir", help="android: the collected Gradle outputs")
    parser.add_argument("--ios-dir", help="ios: the xcframework and template")
    parser.add_argument("--symbols", help="the player's debug information (unstripped libraries, .debug files): the "
                                           "runtime's symbols/, archived on their own (-symbols.zip)")
    parser.add_argument("--out", required=True, help="the players directory the runtime goes into")
    parser.add_argument("--zip", help="also write the runtime's archive into this directory")
    parser.add_argument("--engine-version", help="default: git describe")
    args = parser.parse_args()
    if args.platform in ("linux", "windows", "macos", "web") and not args.build:
        parser.error(f"--build is required for {args.platform}")
    if args.platform == "android" and not args.android_dir:
        parser.error("--android-dir is required for android")
    if args.platform == "ios" and not args.ios_dir:
        parser.error("--ios-dir is required for ios")
    package(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
