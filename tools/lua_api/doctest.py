# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The Lua doc test (S23 R29a): every ```lua block of docs/mcp/guide/*.md, docs/**/lua*.md and docs/scripting/*.md

  - compiles (luac -p when it is on PATH; the MCP test compiles through check_script otherwise),
  - names only members that exist: `Class.member` of a bound class or enum, and `obj:method(...)`
    a method some class has (Lua's string methods too); `self:` calls and local tables are the
    script's own,
  - and, fenced ```lua run, runs in a Play fixture without raising (tools/mcp/tests/test_lua_api_json.py
    through eval_lua).

A failure names the doc file, its line and the unknown symbol:

    docs/mcp/guide/lua-api.md:120: unknown symbol Vector3:nope

    python3 -m lua_api doctest --json build/.../lua-api.json [--root REPO] [files...]
"""

import glob
import os
import re
import shutil
import subprocess

from . import extract as ex

_FENCE = re.compile(r'^\s*```\s*lua(?:\s+(run))?\s*$')
_CLASS_MEMBER = re.compile(r'\b([A-Z][A-Za-z0-9_]*)\s*\.\s*([A-Za-z_]\w*)')
_METHOD_CALL = re.compile(r'([A-Za-z_][\w]*|\)|\])\s*:\s*([A-Za-z_]\w*)\s*[\(\{"\']')
_LUA_STRING_METHODS = {'byte', 'char', 'find', 'format', 'gmatch', 'gsub', 'len', 'lower', 'match', 'rep', 'reverse',
                       'sub', 'upper', 'pack', 'unpack', 'packsize'}
_LUA_LIBRARIES = {'string', 'table', 'math', 'os', 'io', 'coroutine', 'utf8', 'debug', 'package'}


def doc_files(root):
    files = sorted(glob.glob(os.path.join(root, 'docs', 'mcp', 'guide', '*.md')))
    for path in sorted(glob.glob(os.path.join(root, 'docs', '**', '*.md'), recursive=True)):
        scripting = os.path.basename(os.path.dirname(path)) == 'scripting'
        if (os.path.basename(path).lower().startswith('lua') or scripting) and path not in files:
            files.append(path)
    return files


def blocks(path):
    """(first code line, run?, code) of each ```lua block"""
    out = []
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.read().split('\n')
    i = 0
    while i < len(lines):
        m = _FENCE.match(lines[i])
        if not m:
            i += 1
            continue
        start = i + 1
        j = start
        while j < len(lines) and not lines[j].strip().startswith('```'):
            j += 1
        out.append((start + 1, bool(m.group(1)), '\n'.join(lines[start:j])))
        i = j + 1
    return out


def _strip(code):
    """The code with comments and string contents blanked (line structure kept)"""
    out = []
    i = 0
    n = len(code)
    while i < n:
        c = code[i]
        if code.startswith('--', i):
            long = re.match(r'--\[(=*)\[', code[i:])
            if long:
                end = code.find(']' + long.group(1) + ']', i)
                end = n if end < 0 else end + len(long.group(1)) + 2
            else:
                end = code.find('\n', i)
                end = n if end < 0 else end
            out.append(re.sub(r'[^\n]', ' ', code[i:end]))
            i = end
            continue
        if c in '"\'':
            j = i + 1
            while j < n and code[j] != c and code[j] != '\n':
                j += 2 if code[j] == '\\' else 1
            out.append(c + ' ' * (min(j, n) - i - 1) + (c if j < n and code[j] == c else ''))
            i = j + 1 if j < n and code[j] == c else j
            continue
        long = re.match(r'\[(=*)\[', code[i:])
        if long:
            end = code.find(']' + long.group(1) + ']', i)
            end = n if end < 0 else end + len(long.group(1)) + 2
            out.append(re.sub(r'[^\n]', ' ', code[i:end]))
            i = end
            continue
        out.append(c)
        i += 1
    return ''.join(out)


class Api:
    """The names lua-api.json (and its stub fragments, manual symbols) knows"""

    def __init__(self, api):
        self.classes = {c['name']: c for c in api['classes']}
        self.enums = {e['name']: {v['name'] for v in e['values']} for e in api['enums']}
        self.members = {}
        self.methods = set(_LUA_STRING_METHODS)
        for c in api['classes']:
            names = {m['name'] for m in c['methods']} | {p['name'] for p in c['properties']} | \
                    {t['name'] for t in c['enumTables']}
            self.members[c['name']] = names
            self.methods |= {m['name'] for m in c['methods']}
            if c['callOverloads']:
                self.members[c['name']].add('__call')
        for s in api.get('manualSymbols', []):
            self.members.setdefault(s['parent'], set()).add(s['name'])
            if s['kind'] in ('Method', 'StaticMethod'):
                self.methods.add(s['name'])
        # the hand-written stub classes (anime, UIDataModel...)
        for text in api.get('stubFragments', {}).values():
            for cls, name in re.findall(r'^function\s+([A-Za-z_][\w]*)[.:]([A-Za-z_]\w*)\s*\(', text, re.M):
                self.members.setdefault(cls, set()).add(name)
                self.methods.add(name)
            current = None
            for line in text.split('\n'):
                m = re.match(r'^---@class\s+([A-Za-z_][\w.]*)', line)
                if m:
                    current = m.group(1)
                    continue
                m = re.match(r'^---@field\s+([A-Za-z_]\w*)', line)
                if m and current:
                    self.members.setdefault(current, set()).add(m.group(1))
                    if 'fun(' in line:
                        self.methods.add(m.group(1))
                elif not line.startswith('---'):
                    current = None
        for name in ('EditorScript',):
            self.members.setdefault(name, set())
        # the editor scripts' Editor table (EditorScript.lua)
        self.members['Editor'] = {'call', 'selection', 'select', 'log'}

    def has_member(self, cls, member):
        seen = set()
        name = cls
        while name and name not in seen:
            seen.add(name)
            if member in self.members.get(name, set()):
                return True
            c = self.classes.get(name)
            name = c['base'] if c else None
        return False

    def known(self, name):
        return name in self.classes or name in self.enums or name in self.members


def unknown_symbols(code, api):
    """(line in block, symbol) of each member the API does not have"""
    found = []
    text = _strip(code)
    locals_ = set(re.findall(r'\blocal\s+(?:function\s+)?([A-Za-z_]\w*)', text))
    locals_ |= set(re.findall(r'\bfunction\s+([A-Za-z_]\w*)\s*[.:]', text))
    for m in _CLASS_MEMBER.finditer(text):
        cls, member = m.group(1), m.group(2)
        if cls in locals_ or cls in _LUA_LIBRARIES or not api.known(cls):
            continue
        if cls in api.enums:
            ok = member in api.enums[cls] or api.has_member(cls, member)
        else:
            ok = api.has_member(cls, member)
        if not ok:
            found.append((text.count('\n', 0, m.start()) + 1, f'{cls}.{member}'))
    # methods the block defines itself (a script module's)
    defined = set(re.findall(r'\bfunction\s+[A-Za-z_][\w.]*[.:]([A-Za-z_]\w*)', text))
    defined |= set(re.findall(r'\.\s*([A-Za-z_]\w*)\s*=\s*function\b', text))
    for m in _METHOD_CALL.finditer(text):
        receiver, method = m.group(1), m.group(2)
        if receiver == 'self' or method in defined:
            continue
        if api.known(receiver) and receiver[0].isupper() and receiver not in locals_:
            ok = api.has_member(receiver, method)
        else:
            ok = method in api.methods
        if not ok:
            found.append((text.count('\n', 0, m.start()) + 1, f'{receiver}:{method}'))
    return found


def compile_errors(code):
    """luac -p's message ("" when it compiles), None without luac"""
    luac = shutil.which('luac') or shutil.which('luac5.4')
    if not luac:
        return None
    result = subprocess.run([luac, '-p', '-'], input=code.encode('utf-8'), capture_output=True)
    return '' if result.returncode == 0 else result.stderr.decode('utf-8', 'replace').strip()


def check(files, api_json, root, compiler=compile_errors):
    """The failures "path:line: message" of the doc files"""
    api = Api(api_json)
    failures = []
    for path in files:
        rel = os.path.relpath(path, root).replace(os.sep, '/')
        for line, _run, code in blocks(path):
            error = compiler(code) if compiler else None
            if error:
                m = re.search(r':(\d+):', error)
                at = line + int(m.group(1)) - 1 if m else line
                failures.append(f'{rel}:{at}: does not compile: {error}')
            for offset, symbol in unknown_symbols(code, api):
                failures.append(f'{rel}:{line + offset - 1}: unknown symbol {symbol}')
    return failures


def runtime_blocks(files, root):
    """(path:line, code) of the ```lua run blocks"""
    out = []
    for path in files:
        rel = os.path.relpath(path, root).replace(os.sep, '/')
        for line, run, code in blocks(path):
            if run:
                out.append((f'{rel}:{line}', code))
    return out


def main(args):
    if not args.json:
        raise SystemExit('doctest needs --json (the lua-api.json of a build)')
    api = ex.load(args.json)
    files = [os.path.abspath(f) for f in args.files] if args.files else doc_files(args.root)
    failures = check(files, api, args.root)
    for failure in failures:
        print(failure)
    print(f'[lua_api doctest] {len(files)} file(s), {len(failures)} failure(s)')
    return 1 if failures else 0
