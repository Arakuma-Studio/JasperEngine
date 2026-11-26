#!/usr/bin/env python3
"""Name the frames of a JasperEngine crash or hang report (docs/crash-diagnostics.md).

    python3 -I tools/crash/symbolicate.py REPORT [--symbols DIR] [--binary-dir DIR ...]
                                                [--symbol-map FILE] [--json]

REPORT is a crash.txt or hang-NNN.txt (v1), or a report folder (its crash.txt, else the newest
hang report). Each frame "#N <module>+0x<offset> [<symbol>]" is resolved against the module's
file: --binary-dir directories first, then the S30 symbol layout <symbols>/<platform>/<build_id>/
(a .debug / .dSYM / .pdb next to the binary there), then the path the report's modules section
names; a frame of a library loaded after the handler was installed ("module?+0x<address>") is
placed through the report's raw /proc/self/maps copy. ELF and Mach-O frames go through llvm-symbolizer (else addr2line, else atos on macOS);
frames after the first are return addresses, so one byte before is looked up. Web frames
("wasm-function[N]") are named from the emscripten --emit-symbol-map file. A Windows report
prints the cdb / Visual Studio steps for its crash.dmp instead.

    python3 -I tools/crash/symbolicate.py --group DIR [--symbols DIR]... [--binary-dir DIR]...
                                         [--symbol-map FILE] [--by version|platform] [--kinds k,...]
                                         [--since ISO date] [--json] [--top N]

--group reads every report folder under DIR (to depth 3: a folder holding crash.txt, hang-*.txt,
user.txt or session.json) and merges similar ones by signature: the kind, the signal or exception
and the first 3 frames of the game or engine module (docs/crash-diagnostics.md, "Grouping
reports"; the rules are tools/crash/fixtures/group/expected.json's, which the editor's
CrashGroups passes too). Each group prints its count, versions, platforms, first and last seen,
example folders and the newest report's top frames. A frame is named only from a file whose build
id is the report's: --symbols trees are indexed by build id (S28's <platform>/<build_id>/ or S30's
release symbols/<target>/, or any tree), and a file of another build is never used.

Standard library only; safe under `python3 -I` (it imports nothing from the report's folder).
Exit 0 when at least one frame was named (--group: one report was read), 1 when none could be,
2 on a bad argument.
"""

from __future__ import annotations

import argparse
import calendar
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

FRAME = re.compile(r"^#(?P<index>\d+)\s+(?P<module>\S+?)\+0x(?P<offset>[0-9a-fA-F]+)(?:\s+\[(?P<symbol>.*)\])?\s*$")
WASM_FRAME = re.compile(r"wasm-function\[(?P<index>\d+)\]")
# /proc/self/maps: start-end perms offset dev inode path
MAPS_LINE = re.compile(r"^[0-9a-fA-F]+-[0-9a-fA-F]+\s+\S+\s+[0-9a-fA-F]+\s+\S+\s+\d+\s+(?P<path>/.+)$")


def read_report(path: Path) -> tuple[Path, str]:
    if path.is_dir():
        crash = path / "crash.txt"
        if crash.is_file():
            return crash, crash.read_text(errors="replace")
        hangs = sorted(path.glob("hang-*.txt"))
        if not hangs:
            raise FileNotFoundError(f"{path} has no crash.txt or hang-NNN.txt")
        return hangs[-1], hangs[-1].read_text(errors="replace")
    return path, path.read_text(errors="replace")


def header_value(text: str, key: str) -> str:
    prefix = key + ": "
    for line in text.splitlines():
        if line.startswith(prefix):
            return line[len(prefix):].strip()
    return ""


def mappings(text: str) -> list[tuple[int, int, int, str]]:
    """(start, end, file offset, path) of the modules section's /proc/self/maps lines"""
    out = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 6 and "-" in parts[0] and parts[5].startswith("/"):
            try:
                start, end = (int(x, 16) for x in parts[0].split("-"))
                out.append((start, end, int(parts[2], 16), parts[5]))
            except ValueError:
                continue
    return out


def resolve_unknown(address: int, maps: list[tuple[int, int, int, str]]) -> tuple[str, int] | None:
    """A frame outside the modules known at install ("module?+0x<absolute>", a library loaded
    later): its module and module-relative address from the raw maps copy"""
    for start, end, offset, path in maps:
        if start <= address < end:
            # the image's base is its offset-0 mapping; else this segment's start - file offset
            base = next((s for s, _, o, p in maps if p == path and o == 0), start - offset)
            return path, address - base
    return None


def module_paths(text: str) -> dict[str, str]:
    """basename -> full path, from the report's modules section"""
    paths: dict[str, str] = {}
    in_modules = False
    for line in text.splitlines():
        if line.strip() == "modules:":
            in_modules = True
            continue
        if not in_modules:
            continue
        match = MAPS_LINE.match(line.strip())
        candidate = match.group("path") if match else line.split()[-1] if line.split() else ""
        if candidate.startswith("/"):
            paths.setdefault(os.path.basename(candidate), candidate)
    return paths


def find_module(name: str, known: dict[str, str], binary_dirs: list[Path], symbols: Path | None,
                platform: str, build_id: str) -> Path | None:
    for directory in binary_dirs:
        for candidate in (directory / name, directory / (name + ".debug")):
            if candidate.is_file():
                return candidate
    if symbols and platform and build_id:
        folder = symbols / platform / build_id
        for candidate in (folder / (name + ".debug"), folder / name,
                          folder / (name + ".dSYM") / "Contents" / "Resources" / "DWARF" / name):
            if candidate.is_file():
                return candidate
    if name in known and Path(known[name]).is_file():
        return Path(known[name])
    return None


def run(command: list[str]) -> str:
    try:
        return subprocess.run(command, capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""


def symbolize_parts(module: Path, addresses: list[int]) -> list[tuple[str, str]]:
    """(function, "function (file:line)") per address; ("", "") when the tool could not name it"""
    hexes = [hex(a) for a in addresses]
    if shutil.which("llvm-symbolizer"):
        out = run(["llvm-symbolizer", "--obj=" + str(module), "--demangle", "--functions=linkage", *hexes])
        blocks = [b for b in out.strip().split("\n\n")] if out.strip() else []
        names = []
        for block in blocks:
            lines = block.splitlines()
            function = lines[0] if lines else "??"
            location = lines[1] if len(lines) > 1 else ""
            names.append(("", "") if function == "??" else (function, f"{function} ({location})" if location and not location.startswith("??") else function))
        if len(names) == len(addresses):
            return names
    if shutil.which("addr2line"):
        out = run(["addr2line", "-f", "-C", "-e", str(module), *hexes]).splitlines()
        names = []
        for i in range(0, len(out) - 1, 2):
            function, location = out[i], out[i + 1]
            names.append(("", "") if function == "??" else (function, f"{function} ({location})" if not location.startswith("??") else function))
        if len(names) == len(addresses):
            return names
    if sys.platform == "darwin" and shutil.which("atos"):
        # atos treats a zero load address as unspecified. Give it a synthetic
        # nonzero load address and shift each image-relative offset by the same
        # amount; this works for both executable and dylib preferred addresses.
        out = run(["atos", "-o", str(module), "-l", "0x1",
                   *(hex(a + 1) for a in addresses)]).splitlines()
        if len(out) == len(addresses):
            return [("", "") if line.startswith("0x") else (line.split(" (in ")[0], line) for line in out]
    return [("", "")] * len(addresses)


def symbolize(module: Path, addresses: list[int]) -> list[str]:
    """function (file:line) per address, "" when the tool could not name it"""
    return [shown for _, shown in symbolize_parts(module, addresses)]


def read_symbol_map(path: Path) -> dict[int, str]:
    names: dict[int, str] = {}
    for line in path.read_text(errors="replace").splitlines():
        index, _, name = line.partition(":")
        if index.isdigit():
            names[int(index)] = name
    return names


# ---- grouping (--group). Every rule here has a case in tools/crash/fixtures/group/expected.json,
# which editor/util/CrashGroups.cpp passes too: change both together.

KINDS = ("crash", "hang", "abnormal_exit", "user")
SKIP_EXACT = {"abort", "raise", "std::terminate"}
SKIP_PREFIXES = ("__pthread_kill", "__cxa_", "jasperengine::diag::", "jasperengine::CrashGuard")
WINDOWS_EXCEPTIONS = {
    0xC0000005: "EXCEPTION_ACCESS_VIOLATION", 0xC00000FD: "EXCEPTION_STACK_OVERFLOW",
    0xC0000094: "EXCEPTION_INT_DIVIDE_BY_ZERO", 0xC000008E: "EXCEPTION_FLT_DIVIDE_BY_ZERO",
    0xC000001D: "EXCEPTION_ILLEGAL_INSTRUCTION", 0xC0000096: "EXCEPTION_PRIV_INSTRUCTION",
    0x80000003: "EXCEPTION_BREAKPOINT", 0xC0000006: "EXCEPTION_IN_PAGE_ERROR",
    0xC0000409: "STATUS_STACK_BUFFER_OVERRUN", 0xC0000025: "EXCEPTION_NONCONTINUABLE_EXCEPTION",
    0xC000000D: "STATUS_INVALID_PARAMETER",
}
OPERATOR_SYMBOLS = set("+-*/%^&|~!=<>,")
READ_LIMIT = 256 << 10   # frames and the first module line come before this


def is_ident(ch: str) -> bool:
    return ch.isalnum() or ch == "_"


def skip_balanced(text: str, i: int) -> int:
    """index after the bracket group that opens at text[i] ('<' or '(', nested in either)"""
    depth = 0
    while i < len(text):
        if text[i] in "<(":
            depth += 1
        elif text[i] in ">)":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return i


def short_name(symbol: str) -> str:
    """A function's name without return type, arguments or template arguments, namespaces and
    operator names kept, a lambda as <lambda> after its enclosing function (D9)"""
    s = symbol.strip()
    plus = s.rfind("+0x")
    if plus > 0 and all(c in "0123456789abcdefABCDEF" for c in s[plus + 3:]) and len(s) > plus + 3:
        s = s[:plus]
    # lambdas: GCC {lambda(int)#1}, LLVM 'lambda'(int) / 'lambda0'(), MSVC <lambda_1f2e>, clang $_0
    out = []
    i = 0
    while i < len(s):
        if s.startswith("{lambda(", i):
            depth, j = 0, i
            while j < len(s):
                depth += {"{": 1, "}": -1}.get(s[j], 0)
                j += 1
                if depth == 0:
                    break
            out.append("\x02")
            i = j
        elif s.startswith("'lambda", i):
            j = s.find("'", i + 1)
            j = len(s) if j < 0 else j + 1
            if j < len(s) and s[j] == "(":
                j = skip_balanced(s, j)
            out.append("\x02")
            i = j
        elif s.startswith("<lambda_", i):
            j = s.find(">", i)
            out.append("\x02")
            i = len(s) if j < 0 else j + 1
        elif s.startswith("$_", i) and i + 2 < len(s) and s[i + 2].isdigit():
            j = i + 2
            while j < len(s) and s[j].isdigit():
                j += 1
            out.append("\x02")
            i = j
        else:
            out.append(s[i])
            i += 1
    s = "".join(out).replace("(anonymous namespace)", "\x01").replace("`anonymous namespace'", "\x01")
    # drop every <...> and (...), [abi:...] and [clone ...]; keep operator names
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if s.startswith("operator", i) and (i == 0 or not is_ident(s[i - 1])) and \
                (i + 8 == len(s) or not is_ident(s[i + 8])):
            out.append("operator")
            i += 8
            if s.startswith("()", i) or s.startswith("[]", i):
                out.append(s[i:i + 2])
                i += 2
            elif i < len(s) and s[i] in OPERATOR_SYMBOLS:
                while i < len(s) and s[i] in OPERATOR_SYMBOLS:
                    out.append(s[i])
                    i += 1
            elif i + 1 < len(s) and s[i] == " " and (s[i + 1].isalpha() or s[i + 1] == "_"):
                out.append(" ")
                i += 1
                while i < len(s) and (is_ident(s[i]) or s[i] == ":"):
                    out.append(s[i])
                    i += 1
        elif c in "<(":
            i = skip_balanced(s, i)
        elif c == "[":
            if s.startswith("[]", i):
                out.append("[]")
                i += 2
            else:
                j = s.find("]", i)
                i = len(s) if j < 0 else j + 1
        else:
            out.append(c)
            i += 1
    s = " ".join("".join(out).split())
    changed = True
    while changed:
        changed = False
        for suffix in (" const", " volatile", " noexcept", " &&", " &", "&&", "&"):
            if s.endswith(suffix) and len(s) > len(suffix):
                s = s[:-len(suffix)].rstrip()
                changed = True
    tokens = s.split(" ")
    name = tokens[-1] if tokens else ""
    if len(tokens) >= 2 and tokens[-2].endswith("operator"):
        name = tokens[-2] + " " + tokens[-1]
    if name.endswith("\x02::operator()"):
        name = name[:-len("::operator()")]
    return name.replace("\x01", "(anonymous namespace)").replace("\x02", "<lambda>")


def skipped(name: str) -> bool:
    return name in SKIP_EXACT or name.startswith(SKIP_PREFIXES)


def fault_name(text: str) -> str:
    """SIGSEGV, EXCEPTION_ACCESS_VIOLATION, abort, terminate ..."""
    reason = header_value(text, "reason")
    if reason.startswith(("std::terminate", "unhandled exception")):
        return "terminate"
    first = reason.split(" ", 1)[0]
    if first == "SIGABRT":
        return "abort"
    if first.startswith("SIG"):
        return first
    code = header_value(text, "exception_code")
    if code:
        try:
            value = int(code, 16)
            return WINDOWS_EXCEPTIONS.get(value, f"0x{value:08x}")
        except ValueError:
            pass
    signal = header_value(text, "signal")
    return f"signal {signal}" if signal.isdigit() else "unknown"


def module_line_path(line: str) -> str:
    if " size=" in line:   # Windows: <path> <base> size=<size>
        head = line[:line.index(" size=")]
        return head.rsplit(" ", 1)[0] if " " in head else ""
    parts = line.split()
    if len(parts) >= 6 and "-" in parts[0]:   # /proc/self/maps
        path = " ".join(parts[5:])
        return path if path.startswith("/") else ""
    if len(parts) >= 2 and parts[0].startswith("0x"):   # Apple: <base> <path>
        return line.split(None, 1)[1]
    return ""


def main_module(text: str) -> str:
    """the basename of the modules section's first module: the game's executable"""
    seen = False
    for line in text.splitlines():
        line = line.strip()
        if line == "modules:":
            seen = True
        elif seen and line:
            path = module_line_path(line)
            if path:
                return re.split(r"[\\/]", path)[-1]
    return ""


def report_frames(text: str) -> list[dict]:
    frames = []
    for line in text.splitlines():
        match = FRAME.match(line.strip())
        if match:
            frames.append({"index": int(match.group("index")), "module": match.group("module"),
                           "offset": int(match.group("offset"), 16), "symbol": match.group("symbol") or "",
                           "line": line.strip(), "function": "", "resolved": ""})
    return frames


def session_time(report_id: str) -> int:
    match = re.match(r"^(\d{4})(\d{2})(\d{2})T(\d{2})(\d{2})(\d{2})Z", report_id)
    return calendar.timegm(tuple(int(g) for g in match.groups()) + (0, 0, 0)) if match else 0


def iso_time(text: str) -> int:
    match = re.match(r"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})", text)
    return calendar.timegm(tuple(int(g) for g in match.groups()) + (0, 0, 0)) if match else 0


def iso_utc(when: int) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(when))


def read_text(path: Path) -> str:
    try:
        with open(path, "rb") as f:
            return f.read(READ_LIMIT).decode("utf-8", errors="replace")
    except OSError:
        return ""


def read_folder(folder: Path) -> dict:
    """kind, time, version, platform, build id and frames, as CrashHandler's readReport and
    CrashGroups read them"""
    report = {"id": folder.name, "folder": folder, "version": "", "platform": "", "build_id": "",
              "category": "", "text": "", "frames": []}
    hangs = sorted(p for p in folder.glob("hang-*.txt") if len(p.name) > 9)
    when = 0
    if (folder / "crash.txt").is_file():
        report["kind"] = "crash"
        primary = folder / "crash.txt"
        when = int(primary.stat().st_mtime)
    elif (folder / "user.txt").is_file():
        report["kind"] = "user"
        primary = folder / "user.txt"
    elif hangs:
        report["kind"] = "hang"
        primary = hangs[-1]
        when = int(primary.stat().st_mtime)
    else:
        report["kind"] = "abnormal_exit"
        primary = folder / "session.json"
    text = read_text(primary)
    if report["kind"] == "abnormal_exit":
        try:
            marker = json.loads(text)
            report["version"] = str(marker.get("version", "")) if isinstance(marker, dict) else ""
        except ValueError:
            pass
        when = session_time(report["id"])
    else:
        report["text"] = text
        report["version"] = header_value(text, "version")
        report["platform"] = header_value(text, "platform")
        report["build_id"] = header_value(text, "build_id")
        if report["kind"] == "user":
            report["category"] = header_value(text, "category")
            when = iso_time(header_value(text, "time"))
        else:
            report["frames"] = report_frames(text)
    report["time"] = when or session_time(report["id"])
    return report


def signature(report: dict) -> tuple[str, bool, list[dict]]:
    """(signature, unresolved, the frames it names)"""
    kind = report["kind"]
    if kind == "abnormal_exit":
        return "abnormal_exit", False, []
    if kind == "user":
        return "user | " + (report["category"] or "other"), False, []
    text = report["text"]
    if kind == "crash":
        parts = ["crash", fault_name(text)]
    else:
        scopes = header_value(text, "busy_scopes")
        parts = ["hang", header_value(text, "phase") or "unknown"]
        if scopes and scopes != "none":
            parts.append(scopes.split(" > ")[-1])
    main = main_module(text)

    def name_of(frame: dict) -> str:
        return short_name(frame["function"] or frame["symbol"]) if (frame["function"] or frame["symbol"]) else ""

    candidates = [f for f in report["frames"] if not (name_of(f) and skipped(name_of(f)))]
    own = [f for f in candidates if f["module"] == main or f["module"].startswith("libjasperengine")]
    chosen = (own or candidates)[:3]
    unresolved = False
    for frame in chosen:
        name = name_of(frame)
        if not name:
            unresolved = True
            name = f"{frame['module']}+0x{frame['offset']:x}"
        parts.append(name)
    text_out = " | ".join(parts)
    if unresolved:
        text_out += " | build " + (report["build_id"] or "none")
    return text_out, unresolved, chosen


# ---- build ids: a file of another build is never used (R24)

def elf_build_id_location(path: Path) -> tuple[int, int] | None:
    """(file offset, size) of the GNU build id note's bytes"""
    try:
        with open(path, "rb") as f:
            ident = f.read(64)
            if len(ident) < 52 or ident[:4] != b"\x7fELF":
                return None
            wide, end = ident[4] == 2, "<" if ident[5] == 1 else ">"
            if wide:
                phoff, shoff = struct.unpack_from(end + "QQ", ident, 32)
                phentsize, phnum, shentsize, shnum = struct.unpack_from(end + "HHHH", ident, 54)
            else:
                phoff, shoff = struct.unpack_from(end + "II", ident, 28)
                phentsize, phnum, shentsize, shnum = struct.unpack_from(end + "HHHH", ident, 42)
            areas = []
            # the sections first (a .debug file keeps its notes there), else the PT_NOTE segments
            for index in range(min(shnum, 4096)):
                f.seek(shoff + index * shentsize)
                header = f.read(shentsize)
                if len(header) < (64 if wide else 40):
                    break
                if struct.unpack_from(end + "I", header, 4)[0] == 7:   # SHT_NOTE: sh_offset, sh_size
                    offset, size = struct.unpack_from(end + ("QQ" if wide else "II"), header, 24 if wide else 16)
                    areas.append((offset, size))
            if not areas:
                for index in range(min(phnum, 4096)):
                    f.seek(phoff + index * phentsize)
                    header = f.read(phentsize)
                    if len(header) < (56 if wide else 32):
                        break
                    if struct.unpack_from(end + "I", header, 0)[0] == 4:   # PT_NOTE: p_offset, p_filesz
                        offset = struct.unpack_from(end + ("Q" if wide else "I"), header, 8 if wide else 4)[0]
                        size = struct.unpack_from(end + ("Q" if wide else "I"), header, 32 if wide else 16)[0]
                        areas.append((offset, size))
            for offset, size in areas:
                f.seek(offset)
                notes = f.read(min(size, 1 << 16))
                at = 0
                while at + 12 <= len(notes):
                    namesz, descsz, kind = struct.unpack_from(end + "III", notes, at)
                    name_at = at + 12
                    desc_at = name_at + ((namesz + 3) & ~3)
                    if kind == 3 and notes[name_at:name_at + namesz].rstrip(b"\0") == b"GNU" and descsz:
                        return offset + desc_at, descsz
                    at = desc_at + ((descsz + 3) & ~3)
    except (OSError, struct.error):
        return None
    return None


def macho_uuid_locations(path: Path) -> list[tuple[int, int]]:
    """(file offset, 16) of each LC_UUID (one per architecture of a fat file)"""
    found: list[tuple[int, int]] = []
    try:
        with open(path, "rb") as f:
            def thin(base: int) -> None:
                f.seek(base)
                header = f.read(32)
                if len(header) < 28:
                    return
                magic = struct.unpack_from("<I", header)[0]
                if magic in (0xFEEDFACE, 0xFEEDFACF):
                    end = "<"
                elif magic in (0xCEFAEDFE, 0xCFFAEDFE):
                    end = ">"
                else:
                    return
                ncmds = struct.unpack_from(end + "I", header, 16)[0]
                at = base + (32 if magic in (0xFEEDFACF, 0xCFFAEDFE) else 28)
                for _ in range(min(ncmds, 4096)):
                    f.seek(at)
                    command = f.read(8)
                    if len(command) < 8:
                        return
                    cmd, size = struct.unpack(end + "II", command)
                    if cmd == 0x1B:   # LC_UUID
                        found.append((at + 8, 16))
                        return
                    if size < 8:
                        return
                    at += size

            head = f.read(8)
            if len(head) < 8:
                return found
            magic, count = struct.unpack(">II", head)
            if magic in (0xCAFEBABE, 0xCAFEBABF) and 0 < count < 32:
                entry = 32 if magic == 0xCAFEBABF else 20
                for index in range(count):
                    f.seek(8 + index * entry)
                    arch = f.read(entry)
                    offset = struct.unpack_from(">Q", arch, 8)[0] if entry == 32 else struct.unpack_from(">I", arch, 8)[0]
                    thin(offset)
            else:
                thin(0)
    except (OSError, struct.error):
        pass
    return found


def build_id_locations(path: Path) -> list[tuple[int, int]]:
    elf = elf_build_id_location(path)
    return [elf] if elf else macho_uuid_locations(path)


def normal_id(build_id: str) -> str:
    return build_id.replace("-", "").strip().lower()


_ID_CACHE: dict[Path, list[str]] = {}


def build_ids(path: Path) -> list[str]:
    if path not in _ID_CACHE:
        ids = []
        try:
            with open(path, "rb") as f:
                for offset, size in build_id_locations(path):
                    f.seek(offset)
                    ids.append(f.read(size).hex())
        except OSError:
            pass
        _ID_CACHE[path] = ids
    return _ID_CACHE[path]


def index_symbols(roots: list[Path]) -> dict[str, list[Path]]:
    """build id -> the ELF and Mach-O files of the trees that have it (.debug, executables, .so,
    a .dSYM's DWARF), found by their magic, once per run"""
    index: dict[str, list[Path]] = {}
    for root in roots:
        for directory, _, names in os.walk(root):
            for name in sorted(names):
                path = Path(directory) / name
                try:
                    with open(path, "rb") as f:
                        magic = f.read(4)
                except OSError:
                    continue
                if magic == b"\x7fELF" or magic in (b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xce",
                                                    b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"):
                    for build_id in build_ids(path):
                        index.setdefault(build_id, []).append(path)
    return index


def group_module_file(name: str, expected: str, report: dict, binary_dirs: list[Path], symbol_roots: list[Path],
                      index: dict[str, list[Path]]) -> Path | None:
    """the file to name a module's frames from; with an expected build id, only one that has it"""
    for directory in binary_dirs:
        for candidate in (directory / name, directory / (name + ".debug")):
            if candidate.is_file() and (not expected or expected in build_ids(candidate)):
                return candidate
    if expected:
        candidates = index.get(expected, [])
        if candidates:
            # debug information first: a stripped binary of the same build names nothing
            return sorted(candidates, key=lambda p: (not (p.name.endswith(".debug") or ".dSYM" in str(p)),
                                                      p.name != name, str(p)))[0]
        return None
    build_id = report["build_id"]
    if build_id and build_id != "none" and report["platform"]:
        # S28's layout: a library beside the main module's files, in the folder of that build
        for root in symbol_roots:
            folder = root / report["platform"] / build_id
            for candidate in (folder / (name + ".debug"), folder / name):
                if candidate.is_file():
                    return candidate
    return None


def resolve_group_frames(reports: list[dict], binary_dirs: list[Path], symbol_roots: list[Path]) -> None:
    """names the frames the reports do not name: one symbolizer run per module file"""
    if not binary_dirs and not symbol_roots:
        return
    index = index_symbols(symbol_roots) if symbol_roots else {}
    wanted: dict[Path, dict[int, list[dict]]] = {}
    for report in reports:
        main = main_module(report["text"])
        build_id = report["build_id"]
        maps = mappings(report["text"])
        files: dict[str, Path | None] = {}
        for frame in report["frames"]:
            if frame["symbol"]:
                continue
            if frame["module"] == "module?":
                found = resolve_unknown(frame["offset"], maps)
                if not found:
                    continue
                frame["module"], frame["offset"] = os.path.basename(found[0]), found[1]
            name = frame["module"]
            if name not in files:
                expected = normal_id(build_id) if name == main and build_id and build_id != "none" else ""
                files[name] = group_module_file(name, expected, report, binary_dirs, symbol_roots, index)
            module = files[name]
            if module is None:
                continue
            address = frame["offset"] - (1 if frame["index"] > 0 and frame["offset"] > 0 else 0)
            wanted.setdefault(module, {}).setdefault(address, []).append(frame)
    for module, by_address in wanted.items():
        addresses = sorted(by_address)
        for address, (function, shown) in zip(addresses, symbolize_parts(module, addresses)):
            for frame in by_address[address]:
                frame["function"], frame["resolved"] = function, shown


def find_report_folders(root: Path, depth: int = 3) -> list[Path]:
    found = []

    def visit(folder: Path, level: int) -> None:
        try:
            names = set(os.listdir(folder))
        except OSError:
            return
        if names & {"crash.txt", "user.txt", "session.json"} or any(n.startswith("hang-") and n.endswith(".txt") for n in names):
            found.append(folder)
            return
        if level < depth:
            for name in sorted(names):
                child = folder / name
                if child.is_dir() and not child.is_symlink():
                    visit(child, level + 1)

    visit(root, 0)
    return found


def parse_since(text: str) -> int | None:
    when = iso_time(text) or iso_time(text + "T00:00:00")
    return when or None


def group_main(args) -> int:
    root = args.group
    if not root.is_dir():
        print(f"symbolicate: --group {root} is not a folder", file=sys.stderr)
        return 2
    kinds = [k.strip() for k in args.kinds.split(",") if k.strip()] if args.kinds else list(KINDS)
    unknown = [k for k in kinds if k not in KINDS]
    if unknown:
        print(f"symbolicate: --kinds {','.join(unknown)}: use {','.join(KINDS)}", file=sys.stderr)
        return 2
    since = parse_since(args.since) if args.since else None
    if args.since and since is None:
        print(f"symbolicate: --since {args.since}: give a date as 2026-11-01 or 2026-11-01T10:00:00Z", file=sys.stderr)
        return 2
    if args.top is not None and args.top < 1:
        print("symbolicate: --top takes 1 or more", file=sys.stderr)
        return 2

    reports = [r for r in (read_folder(f) for f in find_report_folders(root)) if r["kind"] in kinds and (since is None or r["time"] >= since)]
    if not reports:
        print(f"symbolicate: no report folders under {root}" + (" for those filters" if args.kinds or args.since else ""), file=sys.stderr)
        return 1
    resolve_group_frames(reports, args.binary_dir, args.symbols)

    groups: dict[str, dict] = {}
    for report in reports:
        text, unresolved, frames = signature(report)
        split = report[args.by] or "unknown" if args.by else ""
        group = groups.setdefault(text + "\0" + split, {"signature": text, "kind": report["kind"], "count": 0, "versions": {},
                                                        "platforms": {}, "first_seen": report["time"], "last_seen": report["time"],
                                                        "unresolved": unresolved, "members": []})
        if args.by:
            group[args.by] = split
        group["count"] += 1
        version, platform = report["version"] or "unknown", report["platform"] or "unknown"
        group["versions"][version] = group["versions"].get(version, 0) + 1
        group["platforms"][platform] = group["platforms"].get(platform, 0) + 1
        group["first_seen"] = min(group["first_seen"], report["time"])
        group["last_seen"] = max(group["last_seen"], report["time"])
        group["members"].append((report["time"], report["id"], report, frames))

    out = []
    for group in groups.values():
        members = sorted(group.pop("members"), key=lambda m: (m[0], m[1]), reverse=True)
        newest_frames = members[0][3]
        group["report_ids"] = [m[1] for m in members]
        group["examples"] = [str(m[2]["folder"]) for m in members[:3]]
        group["frames"] = [{"line": f["line"], "name": f["resolved"] or f["symbol"]} for f in newest_frames]
        group["versions"] = dict(sorted(group["versions"].items()))
        group["platforms"] = dict(sorted(group["platforms"].items()))
        out.append(group)
    out.sort(key=lambda g: g["signature"])
    out.sort(key=lambda g: (g["count"], g["last_seen"]), reverse=True)
    if args.top:
        out = out[:args.top]
    for group in out:
        group["first_seen"], group["last_seen"] = iso_utc(group["first_seen"]), iso_utc(group["last_seen"])

    if args.json:
        print(json.dumps({"folder": str(root), "reports": len(reports), "groups": out}, indent=2))
        return 0
    print(f"{len(reports)} reports under {root} in {len(groups)} groups" + (f" (the {len(out)} largest)" if args.top else ""))
    for group in out:
        print()
        print(f"{group['count']} x {group['signature']}")
        split = f"  {args.by}: {group[args.by]}" if args.by else ""
        print(f"  kind: {group['kind']}{split}  versions: " + ", ".join(f"{v} ({n})" for v, n in group["versions"].items()) +
              "  platforms: " + ", ".join(f"{p} ({n})" for p, n in group["platforms"].items()))
        print(f"  first seen {group['first_seen']}, last seen {group['last_seen']}")
        if group["unresolved"]:
            print("  unresolved: frames without names group only within one build; add --symbols <dir> with this build's symbols")
        for example in group["examples"]:
            print(f"  {example}")
        for frame in group["frames"]:
            print(f"    {frame['line']}  =>  {frame['name'] or '?'}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("report", type=Path, nargs="?")
    parser.add_argument("--group", type=Path, metavar="DIR", help="group every report folder under DIR (to depth 3)")
    parser.add_argument("--symbols", type=Path, action="append", default=[],
                        help="a symbols tree: S28's <platform>/<build_id>/, S30's release symbols/<target>/ (--group: repeatable, any tree)")
    parser.add_argument("--binary-dir", type=Path, action="append", default=[], help="where the report's modules are (repeatable)")
    parser.add_argument("--symbol-map", type=Path, help="web: the emscripten --emit-symbol-map file")
    parser.add_argument("--json", action="store_true", help="print the frames (--group: the groups) as JSON")
    parser.add_argument("--by", choices=("version", "platform"), help="--group: split each group by this field")
    parser.add_argument("--kinds", help="--group: only these kinds (crash,hang,abnormal_exit,user)")
    parser.add_argument("--since", help="--group: only reports from this UTC date or time on")
    parser.add_argument("--top", type=int, help="--group: the N largest groups")
    args = parser.parse_args()
    if (args.report is None) == (args.group is None):
        parser.print_usage(sys.stderr)
        print("symbolicate: give a report, or --group DIR", file=sys.stderr)
        return 2
    if args.group is not None:
        return group_main(args)
    if args.by or args.kinds or args.since or args.top is not None:
        print("symbolicate: --by, --kinds, --since and --top go with --group", file=sys.stderr)
        return 2
    # one report: the last --symbols, as before --group made it repeatable
    args.symbols = args.symbols[-1] if args.symbols else None

    try:
        report_file, text = read_report(args.report)
    except (OSError, FileNotFoundError) as error:
        print(f"symbolicate: {error}", file=sys.stderr)
        return 2

    platform = header_value(text, "platform")
    build_id = header_value(text, "build_id")
    if platform.startswith("windows"):
        dump = report_file.parent / "crash.dmp"
        symbols = args.symbols / platform / build_id if args.symbols else Path("<symbol folder>")
        print(f"A Windows report: open {dump} with its .pdb files (build {build_id}).")
        print(f'  cdb -z "{dump}" -y "{symbols}" -c "!analyze -v; .ecxr; k; q"')
        print("  Visual Studio: File > Open > File... the .dmp, Set symbol paths to that folder, Debug with Native Only.")
        print(f"The frames of {report_file.name} name module+offset already; the dump has the rest.")
        return 0

    symbol_map = read_symbol_map(args.symbol_map) if args.symbol_map else {}
    known = module_paths(text)
    frames = []
    for line in text.splitlines():
        match = FRAME.match(line.strip())
        if match:
            frames.append({"index": int(match.group("index")), "module": match.group("module"),
                           "offset": int(match.group("offset"), 16), "symbol": match.group("symbol") or "", "line": line.strip()})
            continue
        wasm = WASM_FRAME.search(line)
        if wasm and symbol_map:
            index = int(wasm.group("index"))
            frames.append({"index": len(frames), "module": "wasm", "offset": index, "symbol": "",
                           "line": line.strip(), "resolved": symbol_map.get(index, "")})

    maps = mappings(text)
    for frame in frames:
        if frame["module"] == "module?":
            found = resolve_unknown(frame["offset"], maps)
            if found:
                path, offset = found
                known.setdefault(os.path.basename(path), path)
                frame["module"], frame["offset"] = os.path.basename(path), offset

    by_module: dict[str, list[dict]] = {}
    for frame in frames:
        if frame["module"] not in ("wasm", "module?"):
            by_module.setdefault(frame["module"], []).append(frame)
    for name, group in by_module.items():
        module = find_module(name, known, args.binary_dir, args.symbols, platform, build_id)
        if module is None:
            for frame in group:
                frame["resolved"] = ""
            continue
        # a return address points after its call: one byte back is inside it
        addresses = [f["offset"] - (1 if f["index"] > 0 and f["offset"] > 0 else 0) for f in group]
        for frame, resolved in zip(group, symbolize(module, addresses)):
            frame["resolved"] = resolved

    named = sum(1 for f in frames if f.get("resolved"))
    if args.json:
        print(json.dumps({"report": str(report_file), "platform": platform, "build_id": build_id, "frames": frames}, indent=2))
    else:
        print(f"{report_file} ({platform}, build {build_id or 'none'}): {named} of {len(frames)} frames named")
        for frame in frames:
            resolved = frame.get("resolved") or frame["symbol"] or "?"
            print(f"  {frame['line']}  =>  {resolved}")
    return 0 if named > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
