#!/usr/bin/env python3
"""
Generates LuaLS (EmmyLua) annotation stubs of the engine's Lua API.

Invoked at build time by CMake, next to generate_api_suggestions.py. It reads the LuaBridge
bindings (engine/core/script/binding/*.cpp) and resolves the C++ types of every bound
function and property from the engine headers (engine/core), then writes:

  - a C++ header embedding the stub files (the editor writes them into every project at
    .jasperengine/engine-api/lua/, see Generator::writeEditorTooling)
  - optionally (--out-dir) the same .lua files, for inspection and tests

One file per Lua class (`Vector3.lua`, `Sprite.lua`...), one per enum namespace
(`CameraType.lua`), plus `Globals.lua` (RegisterEngineEvent, RegisterEvent, NULL_ENTITY,
event objects) and `Script.lua` (the script module contract: ScriptInstance, properties).

Types it cannot resolve become `any` (never a wrong type), and the binding is listed with
`-- unresolved:` in the stub, which tools/mcp/tests/test_stubs.py checks stays empty.

  generate_lua_stubs.py <binding_dir> <engine_core_dir> <output_header> [--out-dir DIR]
"""

import argparse
import os
import re
import sys

# ═══════════════════════════════════════════════════════════════════════════════
# Tokenizer
# ═══════════════════════════════════════════════════════════════════════════════

class Tok:
    __slots__ = ('k', 't', 'line', 'cond')

    def __init__(self, k, t, line, cond):
        self.k = k        # 'id' | 'num' | 'str' | 'chr' | 'op'
        self.t = t
        self.line = line
        self.cond = cond  # tuple of active preprocessor conditions

    def __repr__(self):
        return f'Tok({self.t!r}@{self.line})'


_OPS3 = ('...', '->*', '<=>')
_OPS2 = ('::', '->', '++', '--', '==', '!=', '<=', '>=', '&&', '||', '+=', '-=', '*=', '/=',
         '%=', '&=', '|=', '^=', '##', '.*')


_RE_RAW = re.compile(r'(?:u8|u|U|L)?R"([^()\\\s]{0,16})\(')
_RE_QUOTE = re.compile(r'(?:u8|u|U|L)?(["\'])')
_RE_NUM = re.compile(r"(0[xX][0-9a-fA-F']+|[0-9][0-9']*\.?[0-9']*(?:[eE][+-]?[0-9]+)?|\.[0-9]+(?:[eE][+-]?[0-9]+)?)[a-zA-Z]*")
_RE_IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')


class Source:
    """A tokenized C++ file: tokens, comments by line and which lines hold code."""

    def __init__(self, path, text):
        self.path = path
        self.tokens = []
        self.comments = {}      # line -> comment text (stripped of // and /* */)
        self.full_line = set()  # lines whose comment is the only thing on them
        self.code_lines = set()
        self._tokenize(text)

    def _tokenize(self, s):
        n = len(s)
        i = 0
        line = 1
        cond = ()
        stack = []
        at_line_start = True
        line_has_code = False
        while i < n:
            c = s[i]
            if c == '\n':
                line += 1
                i += 1
                at_line_start = True
                line_has_code = False
                continue
            if c in ' \t\r\f\v':
                i += 1
                continue
            if c == '\\' and i + 1 < n and s[i + 1] == '\n':
                i += 2
                line += 1
                continue
            if at_line_start and c == '#':
                # preprocessor directive, with continuations
                j = i
                text = []
                while j < n and s[j] != '\n':
                    if s[j] == '\\' and j + 1 < n and s[j + 1] == '\n':
                        j += 2
                        line += 1
                        text.append(' ')
                        continue
                    if s.startswith('//', j):
                        while j < n and s[j] != '\n':
                            j += 1
                        break
                    if s.startswith('/*', j):
                        e = s.find('*/', j + 2)
                        e = n if e < 0 else e + 2
                        line += s.count('\n', j, e)
                        j = e
                        continue
                    text.append(s[j])
                    j += 1
                d = ''.join(text)[1:].strip()
                m = re.match(r'(\w+)\s*(.*)', d)
                if m:
                    name, arg = m.group(1), m.group(2).strip()
                    if name in ('if', 'ifdef', 'ifndef'):
                        label = {'if': arg, 'ifdef': f'defined({arg})', 'ifndef': f'!defined({arg})'}[name]
                        stack.append([label])
                        cond = tuple(x[-1] for x in stack)
                    elif name in ('elif', 'else') and stack:
                        prev = stack[-1]
                        label = '!(' + ' || '.join(prev) + ')'
                        if name == 'elif':
                            label += f' && ({arg})'
                        prev.append(label)
                        cond = tuple(x[-1] for x in stack)
                    elif name == 'endif' and stack:
                        stack.pop()
                        cond = tuple(x[-1] for x in stack)
                i = j
                continue
            at_line_start = False
            if s.startswith('//', i):
                j = s.find('\n', i)
                j = n if j < 0 else j
                self._add_comment(line, s[i + 2:j].strip(), not line_has_code)
                i = j
                continue
            if s.startswith('/*', i):
                j = s.find('*/', i + 2)
                j = n if j < 0 else j + 2
                body = s[i + 2:j - 2]
                lines = [l.strip().lstrip('*').strip() for l in body.split('\n')]
                for k, l in enumerate(lines):
                    if l:
                        self._add_comment(line + k, l, not line_has_code if k == 0 else True)
                line += s.count('\n', i, j)
                i = j
                continue
            line_has_code = True
            self.code_lines.add(line)
            # raw string literal
            m = _RE_RAW.match(s, i)
            if m:
                delim = ')' + m.group(1) + '"'
                j = s.find(delim, m.end())
                j = n if j < 0 else j + len(delim)
                self.tokens.append(Tok('str', s[i:j], line, cond))
                line += s.count('\n', i, j)
                i = j
                continue
            m = _RE_QUOTE.match(s, i)
            if m:
                q = m.group(1)
                j = m.end()
                while j < n and s[j] != q:
                    if s[j] == '\\':
                        j += 1
                    j += 1
                j += 1
                self.tokens.append(Tok('str' if q == '"' else 'chr', s[i:j], line, cond))
                i = j
                continue
            if c.isalpha() or c == '_':
                j = _RE_IDENT.match(s, i).end()
                self.tokens.append(Tok('id', s[i:j], line, cond))
                i = j
                continue
            if c.isdigit() or (c == '.' and i + 1 < n and s[i + 1].isdigit()):
                m = _RE_NUM.match(s, i)
                j = m.end()
                self.tokens.append(Tok('num', s[i:j], line, cond))
                i = j
                continue
            for op in _OPS3:
                if s.startswith(op, i):
                    self.tokens.append(Tok('op', op, line, cond))
                    i += 3
                    break
            else:
                for op in _OPS2:
                    if s.startswith(op, i):
                        self.tokens.append(Tok('op', op, line, cond))
                        i += 2
                        break
                else:
                    self.tokens.append(Tok('op', c, line, cond))
                    i += 1

    def _add_comment(self, line, text, full):
        if line in self.comments:
            self.comments[line] += ' ' + text
        else:
            self.comments[line] = text
        if full:
            self.full_line.add(line)

    def doc_before(self, line):
        """The comment block on the lines right above `line` (stops at code or a blank line)."""
        out = []
        l = line - 1
        while l in self.comments and l in self.full_line and l not in self.code_lines:
            out.append(self.comments[l])
            l -= 1
        out.reverse()
        return clean_doc(out)

    def doc_trailing(self, line):
        if line in self.comments and line not in self.full_line:
            return clean_doc([self.comments[line]])
        return []


def clean_doc(lines):
    out = []
    for l in lines:
        l = l.strip()
        if not l or re.fullmatch(r'[-=*/_ ]+', l):
            continue
        if l.startswith(('SPDX-License', '(c) Eduardo')):
            continue
        out.append(l)
    return out


# ═══════════════════════════════════════════════════════════════════════════════
# Token helpers
# ═══════════════════════════════════════════════════════════════════════════════

_OPEN = {'(': ')', '[': ']', '{': '}'}
_CLOSE = {')', ']', '}'}

_NOT_TEMPLATE_BEFORE = {'return', 'if', 'while', 'for', 'switch', 'case', 'operator', 'sizeof',
                        'else', 'do', 'throw', 'new', 'delete', 'co_return', 'and', 'or'}


def skip_balanced(toks, i):
    """toks[i] is an opening bracket: index after its matching close."""
    depth = 0
    n = len(toks)
    while i < n:
        t = toks[i].t
        if t in _OPEN:
            depth += 1
        elif t in _CLOSE:
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def is_template_open(toks, i):
    if i == 0:
        return False
    p = toks[i - 1]
    if p.t == 'template':
        return True
    if p.k != 'id' or p.t in _NOT_TEMPLATE_BEFORE:
        return False
    # an operator name like `operator<` is caught above; a comparison `a < b` would need a
    # matching '>' followed by something a template-id can be followed by
    j = skip_angles(toks, i, check=True)
    return j is not None


def skip_angles(toks, i, check=False):
    """toks[i] is '<' opening template arguments: index after the matching '>'."""
    depth = 0
    n = len(toks)
    while i < n:
        t = toks[i].t
        if t == '<':
            depth += 1
        elif t == '>':
            depth -= 1
            if depth == 0:
                return i + 1
        elif t in ('(', '['):
            i = skip_balanced(toks, i)
            continue
        elif check and t in (';', '{', '}', '&&', '||'):
            return None
        i += 1
    return None if check else n


def split_top(toks, sep=','):
    """Split a token list at top-level separators (outside (), [], {} and template <>)."""
    parts = []
    cur = []
    i = 0
    n = len(toks)
    while i < n:
        t = toks[i].t
        if t in _OPEN:
            j = skip_balanced(toks, i)
            cur.extend(toks[i:j])
            i = j
            continue
        if t == '<' and is_template_open(toks, i):
            j = skip_angles(toks, i)
            cur.extend(toks[i:j])
            i = j
            continue
        if t == sep:
            parts.append(cur)
            cur = []
            i += 1
            continue
        cur.append(toks[i])
        i += 1
    if cur or parts:
        parts.append(cur)
    return parts


def join_tokens(toks):
    """C++ text of a token list with normalized spacing."""
    out = ''
    prev = None
    for tk in toks:
        if prev is not None and ((prev.k in ('id', 'num') and tk.k in ('id', 'num')) or prev.t == ','):
            out += ' '
        out += tk.t
        prev = tk
    return out


# ═══════════════════════════════════════════════════════════════════════════════
# C++ model (headers)
# ═══════════════════════════════════════════════════════════════════════════════

class CppParam:
    __slots__ = ('type', 'name', 'default')

    def __init__(self, type_, name, default):
        self.type = type_
        self.name = name
        self.default = default

    def __repr__(self):
        return f'{self.type} {self.name or ""}'.strip()


class CppFunc:
    __slots__ = ('name', 'ret', 'params', 'static', 'const', 'ctor', 'template', 'tparams', 'doc',
                 'owner', 'variadic')

    def __init__(self, name, ret, params, static=False, const=False, ctor=False, template=False,
                 tparams=None, doc=None, owner=None, variadic=False):
        self.name = name
        self.ret = ret
        self.params = params
        self.static = static
        self.const = const
        self.ctor = ctor
        self.template = template
        self.tparams = tparams or []
        self.doc = doc or []
        self.owner = owner
        self.variadic = variadic

    def __repr__(self):
        return f'{self.ret} {self.name}({", ".join(map(repr, self.params))})'


class CppField:
    __slots__ = ('name', 'type', 'static', 'doc', 'owner')

    def __init__(self, name, type_, static, doc, owner):
        self.name = name
        self.type = type_
        self.static = static
        self.doc = doc
        self.owner = owner


class CppEnum:
    def __init__(self, name, values, doc):
        self.name = name
        self.values = values  # list of (name, int value or None, doc)
        self.doc = doc


class CppClass:
    def __init__(self, name, qual, doc, template=False, tparams=None):
        self.name = name
        self.qual = qual
        self.doc = doc
        self.bases = []
        self.methods = {}
        self.fields = {}
        self.typedefs = {}
        self.template = template
        self.tparams = tparams or []

    def __repr__(self):
        return f'CppClass({self.qual})'


_SPECIFIERS = {'virtual', 'static', 'inline', 'constexpr', 'explicit', 'mutable', 'extern',
               'consteval', 'constinit', 'thread_local', 'register'}
_TYPE_KEYWORDS = {'int', 'float', 'double', 'char', 'bool', 'void', 'long', 'short', 'unsigned',
                  'signed', 'const', 'volatile', 'auto', 'wchar_t', 'char8_t', 'char16_t',
                  'char32_t', 'struct', 'class', 'enum', 'typename'}
_RE_MACRO = re.compile(r'^[A-Z][A-Z0-9_]{2,}$')


class CppModel:
    def __init__(self):
        self.classes = {}    # simple name and Outer::Inner -> [CppClass]
        self.enums = {}      # simple name and Outer::Inner -> CppEnum
        self.typedefs = {}   # name -> type text
        self.functions = {}  # free functions: name -> [CppFunc]
        self.sources = []

    # ── parsing ──────────────────────────────────────────────────────────────

    def parse_file(self, path):
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            src = Source(path, f.read())
        self.sources.append(src)
        self._parse_scope(src, src.tokens, 0, len(src.tokens), None, [])
        return src

    def _register_class(self, cls, outer_names):
        names = {cls.name, '::'.join(outer_names + [cls.name])}
        for nm in names:
            self.classes.setdefault(nm, []).append(cls)

    def _parse_scope(self, src, toks, i, end, cls, outer):
        """Parse declarations in toks[i:end]: a namespace (cls None) or a class body."""
        access_public = True
        if cls is not None:
            access_public = cls._default_public
        while i < end:
            tk = toks[i]
            t = tk.t
            if t == ';':
                i += 1
                continue
            if t in ('public', 'protected', 'private') and i + 1 < end and toks[i + 1].t == ':':
                access_public = t == 'public'
                i += 2
                continue
            if t == 'namespace':
                j = i + 1
                names = []
                while j < end and toks[j].t not in ('{', ';', '='):
                    if toks[j].k == 'id':
                        names.append(toks[j].t)
                    j += 1
                if j < end and toks[j].t == '{':
                    k = skip_balanced(toks, j)
                    self._parse_scope(src, toks, j + 1, k - 1, None, outer)
                    i = k
                else:
                    i = j + 1
                continue
            if t == 'extern' and i + 1 < end and toks[i + 1].k == 'str' and i + 2 < end and toks[i + 2].t == '{':
                k = skip_balanced(toks, i + 2)
                self._parse_scope(src, toks, i + 3, k - 1, cls, outer)
                i = k
                continue
            # template prefix
            template = False
            tparams = []
            start = i
            if t == 'template' and i + 1 < end and toks[i + 1].t == '<':
                j = skip_angles(toks, i + 1)
                for p in split_top(toks[i + 2:j - 1]):
                    ids = [x.t for x in p if x.k == 'id' and x.t not in ('typename', 'class', 'const', 'size_t', 'int', 'bool', 'unsigned')]
                    # `typename T`, `class T = X`, `const T& Value`, `size_t N`
                    pre = [x for x in p if x.t != '=']
                    eq = [k for k, x in enumerate(p) if x.t == '=']
                    head = p[:eq[0]] if eq else p
                    names = [x.t for x in head if x.k == 'id']
                    if names:
                        tparams.append(names[-1])
                template = True
                i = j
                if i >= end:
                    break
                t = toks[i].t
            # nested/top-level type definitions
            if t in ('class', 'struct', 'union'):
                r = self._try_class(src, toks, i, end, outer, start, template, tparams, cls)
                if r is not None:
                    i = r
                    continue
            if t == 'enum':
                r = self._try_enum(src, toks, i, end, outer, start, cls)
                if r is not None:
                    i = r
                    continue
            if t in ('using', 'typedef'):
                j = i
                while j < end and toks[j].t != ';':
                    if toks[j].t == '{':
                        j = skip_balanced(toks, j)
                        continue
                    j += 1
                self._typedef(toks[i:j], cls)
                i = j + 1
                continue
            if t in ('friend', 'static_assert'):
                j = i
                while j < end and toks[j].t not in (';', '{'):
                    if toks[j].t == '(':
                        j = skip_balanced(toks, j)
                        continue
                    j += 1
                if j < end and toks[j].t == '{':
                    j = skip_balanced(toks, j)
                i = j + 1 if j < end and toks[j].t == ';' else j
                continue
            # a declaration (member or namespace level)
            decl, body, nxt = self._read_decl(toks, i, end)
            if nxt <= i:
                nxt = i + 1
            if decl:
                doc = src.doc_before(toks[start].line) + src.doc_trailing(decl[-1].line)
                if cls is None or access_public:
                    try:
                        self._interpret_decl(decl, cls, doc, template, tparams, body is not None)
                    except Exception:
                        pass
            i = nxt

    def _try_class(self, src, toks, i, end, outer, start, template, tparams, owner):
        kind = toks[i].t
        j = i + 1
        name = None
        while j < end:
            tk = toks[j]
            if tk.t in ('{', ':', ';') or tk.t == 'final':
                break
            if tk.t == '[' and j + 1 < end and toks[j + 1].t == '[':
                j = skip_balanced(toks, j)
                continue
            if tk.t == '<':
                j = skip_angles(toks, j)  # partial specialization
                continue
            if tk.k == 'id':
                if _RE_MACRO.match(tk.t) and j + 1 < end and toks[j + 1].k == 'id':
                    j += 1
                    continue
                if name is not None and toks[j - 1].t != '::':
                    return None  # `struct X y;` style declaration
                name = tk.t
            elif tk.t != '::':
                return None
            j += 1
        if name is None or j >= end:
            return None
        if toks[j].t == 'final':
            j += 1
        if toks[j].t == ';':
            return j + 1  # forward declaration
        bases = []
        if toks[j].t == ':':
            k = j + 1
            while k < end and toks[k].t != '{':
                if toks[k].t == '<':
                    k = skip_angles(toks, k)
                    continue
                if toks[k].t in ('(', ';'):
                    return None
                k += 1
            for part in split_top(toks[j + 1:k]):
                ids = [x for x in part if x.t not in ('public', 'protected', 'private', 'virtual')]
                if ids:
                    bases.append(normalize_type(join_tokens(ids)))
            j = k
        if j >= end or toks[j].t != '{':
            return None
        k = skip_balanced(toks, j)
        qual = '::'.join(outer + [name])
        doc = src.doc_before(toks[start].line)
        cls = CppClass(name, qual, doc, template, tparams)
        cls.bases = bases
        cls._default_public = kind != 'class'
        self._register_class(cls, outer)
        self._parse_scope(src, toks, j + 1, k - 1, cls, outer + [name])
        # `struct {...} name;` variable declarations after the body are ignored
        while k < end and toks[k].t != ';':
            k += 1
        return k + 1

    def _try_enum(self, src, toks, i, end, outer, start, owner):
        j = i + 1
        if j < end and toks[j].t in ('class', 'struct'):
            j += 1
        name = None
        if j < end and toks[j].k == 'id':
            name = toks[j].t
            j += 1
        if j < end and toks[j].t == ':':
            while j < end and toks[j].t not in ('{', ';'):
                j += 1
        if j >= end or toks[j].t != '{':
            if j < end and toks[j].t == ';':
                return j + 1
            return None
        k = skip_balanced(toks, j)
        values = []
        known = {}
        cur = -1
        for part in split_top(toks[j + 1:k - 1]):
            if not part:
                continue
            vname = part[0].t
            vdoc = src.doc_before(part[0].line) + src.doc_trailing(part[-1].line)
            if len(part) > 2 and part[1].t == '=':
                val = eval_int(part[2:], known, self)
                cur = val if val is not None else None
            else:
                cur = cur + 1 if cur is not None else None
            if cur is not None:
                known[vname] = cur
            values.append((vname, cur, vdoc))
        if name:
            en = CppEnum(name, values, src.doc_before(toks[start].line))
            self.enums.setdefault(name, en)
            self.enums['::'.join(outer + [name])] = en
        while k < end and toks[k].t != ';':
            k += 1
        return k + 1

    def _typedef(self, toks, cls):
        if not toks:
            return
        target = cls.typedefs if cls is not None else self.typedefs
        if toks[0].t == 'using':
            if len(toks) >= 4 and toks[1].k == 'id' and toks[2].t == '=':
                target[toks[1].t] = normalize_type(join_tokens(toks[3:]))
        else:
            body = toks[1:]
            if body and body[-1].k == 'id':
                if any(x.t == '(' for x in body):
                    return  # function pointer typedef
                target[body[-1].t] = normalize_type(join_tokens(body[:-1]))

    def _read_decl(self, toks, i, end):
        """One declaration: (tokens without body, (body start, end) or None, next index)."""
        start = i
        saw_eq = False
        saw_paren = False
        while i < end:
            t = toks[i].t
            if t == ';':
                return toks[start:i], None, i + 1
            if t == '(' or t == '[':
                if t == '(' and not saw_eq:
                    saw_paren = True
                i = skip_balanced(toks, i)
                continue
            if t == '{':
                if saw_eq or not saw_paren:
                    i = skip_balanced(toks, i)
                    if not saw_paren and not saw_eq:
                        # `T name{init};` or a construct we do not understand
                        pass
                    continue
                j = skip_balanced(toks, i)
                nxt = j + 1 if j < end and toks[j].t == ';' else j
                return toks[start:i], (i, j), nxt
            if t == '}':
                return toks[start:i], None, i + 1
            if t == '=' and not (i > start and toks[i - 1].t == 'operator'):
                saw_eq = True
            elif t == '<' and is_template_open(toks, i):
                i = skip_angles(toks, i)
                continue
            elif t == ':' and saw_paren and not saw_eq:
                # constructor initializer list
                i += 1
                while i < end:
                    while i < end and (toks[i].k == 'id' or toks[i].t in ('::', '<', '>')):
                        if toks[i].t == '<':
                            i = skip_angles(toks, i)
                            continue
                        i += 1
                    if i < end and toks[i].t in ('(', '{'):
                        i = skip_balanced(toks, i)
                    if i < end and toks[i].t == ',':
                        i += 1
                        continue
                    break
                if i < end and toks[i].t == '{':
                    j = skip_balanced(toks, i)
                    return toks[start:i], (i, j), j
                continue
            i += 1
        return toks[start:end], None, end

    def _interpret_decl(self, decl, cls, doc, template, tparams, has_body):
        toks = list(decl)
        # attributes
        while toks and toks[0].t == '[' and len(toks) > 1 and toks[1].t == '[':
            toks = toks[skip_balanced(toks, 0):]
        static = False
        while toks and (toks[0].t in _SPECIFIERS or (_RE_MACRO.match(toks[0].t) and len(toks) > 1 and toks[1].k == 'id')):
            if toks[0].t == 'static':
                static = True
            toks = toks[1:]
        if not toks:
            return
        # find the top-level '(' of a function declarator (before any top-level '=')
        k = 0
        paren = None
        n = len(toks)
        while k < n:
            t = toks[k].t
            if t == '=' and not (k > 0 and toks[k - 1].t == 'operator'):
                break
            if t == '<' and is_template_open(toks, k):
                k = skip_angles(toks, k)
                continue
            if t == '(' and not (k > 0 and toks[k - 1].t == 'operator' and k + 1 < n and toks[k + 1].t == ')'):
                paren = k
                break
            if t in ('[', '{'):
                k = skip_balanced(toks, k)
                continue
            k += 1
        if paren is not None:
            self._interpret_function(toks, paren, cls, doc, static, template, tparams)
        else:
            self._interpret_fields(toks, cls, doc, static)

    def _interpret_function(self, toks, paren, cls, doc, static, template, tparams):
        if paren + 1 < len(toks) and toks[paren + 1].t in ('*', '&', '^'):
            return  # function pointer variable
        # operator names
        op_idx = None
        for k in range(paren):
            if toks[k].t == 'operator':
                op_idx = k
                break
        if op_idx is not None:
            if toks[op_idx + 1].t == '(' and op_idx + 2 < len(toks) and toks[op_idx + 2].t == ')':
                name = 'operator()'
                paren = op_idx + 3
            else:
                name = 'operator' + ''.join(x.t for x in toks[op_idx + 1:paren])
            ret_toks = toks[:op_idx]
        else:
            if paren == 0 or toks[paren - 1].k != 'id':
                return
            name = toks[paren - 1].t
            ret_toks = toks[:paren - 1]
            if ret_toks and ret_toks[-1].t == '~':
                return  # destructor
            if ret_toks and ret_toks[-1].t == '::':
                return  # out-of-class definition
        close = skip_balanced(toks, paren)
        params, variadic = parse_params(toks[paren + 1:close - 1])
        rest = toks[close:]
        const = any(x.t == 'const' for x in rest[:3])
        ret = normalize_type(join_tokens(ret_toks)) if ret_toks else ''
        if ret == 'auto':
            for k, x in enumerate(rest):
                if x.t == '->':
                    tail = []
                    for y in rest[k + 1:]:
                        if y.t in ('{', ';', '=', 'override', 'final'):
                            break
                        tail.append(y)
                    ret = normalize_type(join_tokens(tail))
                    break
        if any(x.t == '=' for x in rest) and any(x.t == 'delete' for x in rest):
            return
        ctor = cls is not None and name == cls.name and not ret_toks
        if not ret_toks and not ctor and op_idx is None:
            return  # a macro invocation or something else
        fn = CppFunc(name, ret, params, static=static, const=const, ctor=ctor, template=template,
                     tparams=tparams, doc=doc, owner=cls, variadic=variadic)
        if cls is not None:
            cls.methods.setdefault(name, []).append(fn)
        else:
            self.functions.setdefault(name, []).append(fn)

    def _interpret_fields(self, toks, cls, doc, static):
        if cls is None:
            return
        parts = split_top(toks)
        base_type = None
        for idx, part in enumerate(parts):
            # declarator name: the identifier before the first top-level '=', '[', '{' or ':'
            k = 0
            stop = len(part)
            while k < len(part):
                t = part[k].t
                if t in ('=', '[', '{', ':') and not (t == ':' and k + 1 < len(part) and part[k + 1].t == ':'):
                    stop = k
                    break
                if t == '<' and is_template_open(part, k):
                    k = skip_angles(part, k)
                    continue
                if t == '(':
                    k = skip_balanced(part, k)
                    continue
                k += 1
            head = part[:stop]
            if not head or head[-1].k != 'id':
                return
            name = head[-1].t
            if idx == 0:
                type_toks = head[:-1]
                if not type_toks:
                    return
                base_type = normalize_type(join_tokens([x for x in type_toks if x.t not in ('*', '&')]))
                ftype = normalize_type(join_tokens(type_toks))
            else:
                ptr = ''.join(x.t for x in head[:-1] if x.t in ('*', '&'))
                ftype = base_type + ptr
            if name in _TYPE_KEYWORDS:
                return
            cls.fields[name] = CppField(name, ftype, static, doc, cls)

    # ── lookups ──────────────────────────────────────────────────────────────

    def find_class(self, name):
        name = strip_ns(name)
        base = re.sub(r'<.*$', '', name)
        lst = self.classes.get(base)
        if not lst:
            return None
        # prefer a definition with members
        best = max(lst, key=lambda c: len(c.methods) + len(c.fields))
        return best

    def mro(self, cls, seen=None):
        if cls is None:
            return []
        seen = seen if seen is not None else set()
        if id(cls) in seen:
            return []
        seen.add(id(cls))
        out = [cls]
        for b in cls.bases:
            out.extend(self.mro(self.find_class(b), seen))
        return out

    def find_methods(self, cls_name, name):
        """Overloads of name visible in cls_name: from the first class in its hierarchy declaring it."""
        cls = self.find_class(cls_name)
        for c in self.mro(cls):
            if name in c.methods:
                return c.methods[name]
        return []

    def find_field(self, cls_name, name):
        cls = self.find_class(cls_name)
        for c in self.mro(cls):
            if name in c.fields:
                return c.fields[name]
        return None

    def find_enum(self, name):
        name = strip_ns(name)
        return self.enums.get(name)

    def resolve_typedef(self, name, cls_name=None, depth=0):
        """Underlying type of a typedef name, or None."""
        if depth > 8:
            return None
        if '::' in name:
            outer, inner = name.rsplit('::', 1)
            c = self.find_class(outer)
            if c is not None:
                for k in self.mro(c):
                    if inner in k.typedefs:
                        return k.typedefs[inner]
        if cls_name:
            c = self.find_class(cls_name)
            for k in self.mro(c):
                if name in k.typedefs:
                    return k.typedefs[name]
        return self.typedefs.get(name)


def parse_params(toks):
    params = []
    variadic = False
    if not toks or (len(toks) == 1 and toks[0].t == 'void'):
        return params, variadic
    for part in split_top(toks):
        if not part:
            continue
        if part[-1].t == '...' or (len(part) == 1 and part[0].t == '...'):
            variadic = True
            continue
        default = None
        eq = None
        depth = 0
        for k, x in enumerate(part):
            if x.t in _OPEN:
                depth += 1
            elif x.t in _CLOSE:
                depth -= 1
            elif x.t == '=' and depth == 0:
                eq = k
                break
        if eq is not None:
            default = join_tokens(part[eq + 1:])
            part = part[:eq]
        # array parameter
        if part and part[-1].t == ']':
            k = len(part) - 1
            while k >= 0 and part[k].t != '[':
                k -= 1
            part = part[:k]
            arr = '*'
        else:
            arr = ''
        name = None
        core = [x for x in part if x.t not in ('const', 'volatile')]
        if len(core) >= 2 and part[-1].k == 'id' and part[-1].t not in _TYPE_KEYWORDS \
                and part[-2].t != '::' and part[-2].t not in ('struct', 'class', 'enum', 'typename'):
            name = part[-1].t
            part = part[:-1]
        params.append(CppParam(normalize_type(join_tokens(part)) + arr, name, default))
    return params, variadic


def eval_int(toks, known, model):
    text = ''
    prev = None
    for tk in toks:
        if tk.k == 'num':
            v = re.sub(r"[uUlLfF]+$", '', tk.t.replace("'", ''))
            if prev is not None and prev.k in ('id', 'num'):
                text += ' '
            text += v
        elif tk.k == 'id':
            if prev is not None and prev.k in ('id', 'num'):
                text += ' '
            text += tk.t
        else:
            text += tk.t
        prev = tk
    text = text.replace('::', '__')
    names = {}
    for k, v in known.items():
        names[k] = v
    for en in list(model.enums.values()):
        for vn, vv, _ in en.values:
            if vv is not None:
                names.setdefault(f'{en.name}__{vn}', vv)
    text = re.sub(r'\b(?:static_cast|uint32_t|int32_t|uint64_t|int|unsigned)\b', '', text)
    text = text.replace('<>', '')
    try:
        val = eval(text, {'__builtins__': {}}, names)  # constant expressions of enum headers only
    except Exception:
        return None
    if isinstance(val, int):
        return val
    return None


_STD_STRIP = re.compile(r'\b(?:jasperengine|luabridge)::')


def strip_ns(t):
    return _STD_STRIP.sub('', t).strip()


def normalize_type(t):
    t = t.strip()
    t = re.sub(r'\b(?:typename|struct|class|enum)\s+', '', t)
    t = strip_ns(t)
    t = re.sub(r'\s+', ' ', t)
    t = re.sub(r'\s*([*&<>(),])\s*', r'\1', t)
    t = t.replace(',', ', ')
    t = re.sub(r'\b(const|volatile)(?=[A-Za-z_])', r'\1 ', t)
    return t


def type_key(t):
    """Comparison key of a parameter type (drops const, references and spacing)."""
    t = normalize_type(t)
    t = re.sub(r'\bconst\b', '', t)
    t = t.replace('&', '').replace(' ', '')
    t = t.replace('std::', '')
    return t


def bare_type(t):
    """(base type text, pointer count) with cv-qualifiers and references removed."""
    t = normalize_type(t)
    t = re.sub(r'\b(?:const|volatile)\b', '', t).strip()
    t = t.replace('&&', '').replace('&', '').strip()
    stars = 0
    while t.endswith('*'):
        stars += 1
        t = t[:-1].strip()
    return t.replace(' >', '>'), stars


# ═══════════════════════════════════════════════════════════════════════════════
# Bindings
# ═══════════════════════════════════════════════════════════════════════════════

class Call:
    """One `.method<targs>(args)` link of a LuaBridge chain."""
    __slots__ = ('name', 'targs', 'args', 'line', 'cond', 'doc', 'src')

    def __init__(self, name, targs, args, line, cond, doc, src):
        self.name = name
        self.targs = targs
        self.args = args
        self.line = line
        self.cond = cond
        self.doc = doc
        self.src = src


class LuaMember:
    def __init__(self, kind, name, call, cls):
        self.kind = kind    # Method | StaticMethod | Property | StaticProperty
        self.name = name
        self.call = call
        self.cls = cls
        self.sigs = []      # list of Sig (functions)
        self.prop_type = None
        self.writable = False
        self.copy = False
        self.event = None   # FunctionSubscribe Lua class when the property is an event
        self.doc = []
        self.unresolved = []


class LuaClass:
    def __init__(self, name, cpp, base_cpp, call, file):
        self.name = name
        self.cpp = cpp
        self.base_cpp = base_cpp
        self.base = None
        self.call = call
        self.file = file
        self.members = []
        self.ctors = []     # list of Sig
        self.doc = []
        self.cond = call.cond


class LuaEnum:
    def __init__(self, name, call, file):
        self.name = name
        self.call = call
        self.file = file
        self.values = []    # (name, value expr tokens, call)
        self.cpp_enum = None
        self.doc = []


class Sig:
    __slots__ = ('params', 'ret', 'doc', 'cfunc', 'source')

    def __init__(self, params, ret, doc=None, cfunc=False, source=''):
        self.params = params    # [(cpp type, name or None)], Lua-visible only
        self.ret = ret          # cpp type, '' for void, None for unknown
        self.doc = doc or []
        self.cfunc = cfunc
        self.source = source

    def __repr__(self):
        return f'Sig({self.params} -> {self.ret!r})'


class Bindings:
    def __init__(self, model):
        self.model = model
        self.classes = []
        self.enums = []
        self.globals = []          # (name, value tokens, call)
        self.local_model = None    # free functions of the binding files

    def parse_file(self, path):
        model = self.model
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            text = f.read()
        src = Source(path, text)
        # free functions defined in the binding file (lua_CFunctions, helpers, templates)
        local = CppModel()
        local.classes = model.classes
        local.enums = model.enums
        local.typedefs = model.typedefs
        local._parse_scope(src, src.tokens, 0, len(src.tokens), None, [])
        for k, v in local.functions.items():
            model.functions.setdefault('@' + os.path.basename(path) + ':' + k, []).extend(v)
        file = os.path.basename(path)
        toks = src.tokens
        n = len(toks)
        i = 0
        while i < n:
            if toks[i].t == 'getGlobalNamespace' and i >= 2 and toks[i - 1].t == '::' and toks[i - 2].t == 'luabridge':
                chain_doc = src.doc_before(toks[i - 2].line)
                i = self._parse_chain(src, toks, i + 1, file, chain_doc)
                continue
            i += 1

    def _parse_chain(self, src, toks, i, file, chain_doc):
        n = len(toks)
        i = skip_balanced(toks, i)  # (L)
        calls = []
        while i < n and toks[i].t == '.':
            name_tok = toks[i + 1]
            j = i + 2
            targs = []
            if j < n and toks[j].t == '<':
                k = skip_angles(toks, j)
                targs = split_top(toks[j + 1:k - 1])
                j = k
            if j >= n or toks[j].t != '(':
                break
            k = skip_balanced(toks, j)
            args = split_top(toks[j + 1:k - 1])
            doc = src.doc_before(name_tok.line)
            calls.append(Call(name_tok.t, targs, args, name_tok.line, name_tok.cond, doc, src))
            i = k
        ns = None
        cls = None
        for c in calls:
            if c.name == 'beginNamespace':
                ns = LuaEnum(literal(c.args[0]), c, file)
                ns.doc = chain_doc + c.doc
            elif c.name == 'endNamespace':
                if ns is not None and ns.values:
                    self.enums.append(ns)
                ns = None
            elif c.name in ('beginClass', 'deriveClass'):
                cpp = normalize_type(join_tokens(c.targs[0]))
                base = normalize_type(join_tokens(c.targs[1])) if c.name == 'deriveClass' else None
                cls = LuaClass(literal(c.args[0]), cpp, base, c, file)
                cls.doc = chain_doc + c.doc
            elif c.name == 'endClass':
                if cls is not None:
                    self.classes.append(cls)
                cls = None
            elif c.name == 'addVariable':
                if ns is not None:
                    ns.values.append((literal(c.args[0]), c.args[1], c))
                elif cls is None:
                    self.globals.append((literal(c.args[0]), c.args[1], c))
            elif cls is not None:
                if c.name == 'addConstructor':
                    cls.ctors.append(c)
                elif c.name in ('addFunction', 'addStaticFunction', 'addProperty', 'addStaticProperty'):
                    kind = {'addFunction': 'Method', 'addStaticFunction': 'StaticMethod',
                            'addProperty': 'Property', 'addStaticProperty': 'StaticProperty'}[c.name]
                    cls.members.append(LuaMember(kind, literal(c.args[0]), c, cls))
        return i


def literal(arg):
    if arg and arg[0].k == 'str':
        s = arg[0].t
        return s[s.index('"') + 1:s.rindex('"')]
    return join_tokens(arg)


# ═══════════════════════════════════════════════════════════════════════════════
# Callable resolution
# ═══════════════════════════════════════════════════════════════════════════════

class Resolver:
    def __init__(self, model, bindings):
        self.model = model
        self.b = bindings
        self.cpp_to_lua = {}
        for c in bindings.classes:
            self.cpp_to_lua[type_key(c.cpp)] = c.name
        self.lua_classes = {c.name: c for c in bindings.classes}
        self.enum_ns = {}  # C++ enum name -> Lua enum name

    # ── free functions in the binding file ───────────────────────────────────

    def local_functions(self, file, name):
        return self.model.functions.get('@' + file + ':' + name, [])

    # ── expressions ──────────────────────────────────────────────────────────

    def resolve(self, toks, member, hint=None):
        """Signatures (C++ level, self included) of one callable argument.

        Returns (kind, Sig-like dict) where kind is 'member' (self implicit), 'free' (all
        params explicit), 'data' (data member pointer: Sig.ret = its type) or None."""
        toks = list(toks)
        while toks and toks[0].t == '+':
            toks = toks[1:]
        if not toks:
            return None
        t0 = toks[0].t
        # luabridge::overload<Args>(callable) and friends
        if t0 == 'luabridge' or t0 in ('overload', 'constOverload', 'nonConstOverload'):
            k = 0
            while k < len(toks) and toks[k].t != '<':
                k += 1
            if k < len(toks):
                e = skip_angles(toks, k)
                args = [normalize_type(join_tokens(p)) for p in split_top(toks[k + 1:e - 1]) if p]
                inner = toks[e + 1:skip_balanced(toks, e) - 1]
                return self.resolve(inner, member, hint=args)
        # C-style cast to a function pointer type
        if t0 == '(':
            e = skip_balanced(toks, 0)
            inner = toks[1:e - 1]
            cast = self.parse_fn_ptr_type(inner)
            if cast is not None:
                return self.apply_cast(cast, toks[e:], member)
            return self.resolve(inner, member, hint)
        if t0 == 'static_cast' and len(toks) > 1 and toks[1].t == '<':
            e = skip_angles(toks, 1)
            cast = self.parse_fn_ptr_type(toks[2:e - 1])
            inner = toks[e + 1:skip_balanced(toks, e) - 1]
            if cast is not None:
                return self.apply_cast(cast, inner, member)
            return self.resolve(inner, member, hint)
        if t0 == '[':
            return self.resolve_lambda(toks, member)
        if t0 == '&':
            toks = toks[1:]
        return self.resolve_name(toks, member, hint)

    def parse_fn_ptr_type(self, toks):
        """`R (C::*)(Args) const` or `R (*)(Args)`: (ret, class or None, [param types], const)."""
        for k, x in enumerate(toks):
            if x.t == '(':
                e = skip_balanced(toks, k)
                inner = toks[k + 1:e - 1]
                if not inner or inner[-1].t != '*':
                    continue
                cls = None
                if len(inner) >= 3 and inner[-2].t == '::':
                    cls = normalize_type(join_tokens(inner[:-2]))
                elif len(inner) != 1:
                    continue
                if e >= len(toks) or toks[e].t != '(':
                    continue
                pe = skip_balanced(toks, e)
                params, _ = parse_params(toks[e + 1:pe - 1])
                const = any(y.t == 'const' for y in toks[pe:])
                ret = normalize_type(join_tokens(toks[:k]))
                return ret, cls, [p.type for p in params], const
        return None

    def apply_cast(self, cast, target, member):
        ret, cls, ptypes, const = cast
        while target and target[0].t in ('&', '+'):
            target = target[1:]
        names = [None] * len(ptypes)
        doc = []
        qual, name = self.split_qualified(target)
        fns = []
        if qual and name:
            fns = self.model.find_methods(qual, name)
        elif name:
            fns = self.model.functions.get(name, [])
        for fn in fns:
            if len(fn.params) == len(ptypes) and all(type_key(a.type) == type_key(b) for a, b in zip(fn.params, ptypes)):
                names = [p.name for p in fn.params]
                doc = fn.doc
                break
        params = list(zip(ptypes, names))
        if cls is not None:
            return 'member', Sig(params, ret, doc, source=f'{cls}::{name}')
        return 'free', Sig(params, ret, doc, source=name or '')

    def split_qualified(self, toks):
        """`A::B::name<targs>` -> ('A::B', 'name') (template args dropped)."""
        parts = []
        cur = []
        k = 0
        while k < len(toks):
            x = toks[k]
            if x.t == '<' and is_template_open(toks, k):
                e = skip_angles(toks, k)
                cur.extend(toks[k:e])
                k = e
                continue
            if x.t == '::':
                parts.append(cur)
                cur = []
            else:
                cur.append(x)
            k += 1
        parts.append(cur)
        if not parts[-1]:
            return None, None
        last = parts[-1]
        name = last[0].t if last else None
        if name == 'operator':
            name = 'operator' + ''.join(x.t for x in last[1:])
        qual = '::'.join(join_tokens(p) for p in parts[:-1])
        return normalize_type(qual) if qual else None, name

    def template_args_of(self, toks):
        """Template arguments of the last name in a qualified id, as token lists."""
        for k in range(len(toks) - 1, -1, -1):
            if toks[k].t == '<' and is_template_open(toks, k):
                e = skip_angles(toks, k)
                if e == len(toks):
                    return split_top(toks[k + 1:e - 1])
        return []

    def resolve_name(self, toks, member, hint):
        qual, name = self.split_qualified(toks)
        if not name:
            return None
        if qual:
            # FunctionSubscribe<...>::call / add
            fns = self.model.find_methods(qual, name)
            if fns:
                fn = self.pick_overload(fns, hint)
                if fn is None:
                    return None
                return ('free' if fn.static else 'member'), self.sig_of(fn, qual)
            fld = self.model.find_field(qual, name)
            if fld is not None:
                return 'data', Sig([], fld.type, fld.doc, source=f'{qual}::{name}')
            return None
        # a free function or template of the binding file (or the engine)
        fns = self.local_functions(member.call.src.path and os.path.basename(member.call.src.path), name) \
            or self.model.functions.get(name, [])
        if not fns:
            return None
        fn = self.pick_overload(fns, hint)
        if fn is None:
            return None
        sig = self.sig_of(fn, None)
        targs = self.template_args_of(toks)
        if fn.template and fn.tparams and targs:
            subst = {}
            for pn, ta in zip(fn.tparams, targs):
                subst[pn] = normalize_type(join_tokens(ta))
            sig = Sig([(substitute(t, subst), n) for t, n in sig.params], substitute(sig.ret, subst) if sig.ret else sig.ret,
                      sig.doc, sig.cfunc, sig.source)
        return 'free', sig

    def pick_overload(self, fns, hint):
        cands = [f for f in fns if not f.ctor]
        if hint is not None:
            hk = [type_key(h) for h in hint]
            for f in cands:
                if len(f.params) == len(hint) and [type_key(p.type) for p in f.params] == hk:
                    return f
            # LuaBridge overload<> lists the exact parameter types: a mismatch here means the
            # header parse missed something, fall back on the arity
            same = [f for f in cands if len(f.params) == len(hint)]
            if same:
                return same[0]
            return None
        non_template = [f for f in cands if not f.template]
        if non_template:
            return non_template[0]
        return cands[0] if cands else None

    def sig_of(self, fn, qual):
        params = [(p.type, p.name) for p in fn.params]
        cfunc = len(fn.params) == 1 and bare_type(fn.params[0].type)[0] == 'lua_State' and \
            re.sub(r'\s', '', fn.ret) in ('int',)
        return Sig(params, fn.ret, fn.doc, cfunc=cfunc, source=(qual + '::' if qual else '') + fn.name)

    def resolve_lambda(self, toks, member):
        e = skip_balanced(toks, 0)  # captures
        k = e
        params = []
        if k < len(toks) and toks[k].t == '(':
            pe = skip_balanced(toks, k)
            params, _ = parse_params(toks[k + 1:pe - 1])
            k = pe
        ret = None
        while k < len(toks) and toks[k].t != '{':
            if toks[k].t == '->':
                tail = []
                k += 1
                while k < len(toks) and toks[k].t != '{':
                    tail.append(toks[k])
                    k += 1
                ret = normalize_type(join_tokens(tail))
                break
            k += 1
        if k >= len(toks):
            return None
        body = toks[k + 1:skip_balanced(toks, k) - 1]
        env = {p.name: p.type for p in params if p.name}
        if ret is None:
            ret = self.infer_return(body, env, member)
        sig = Sig([(p.type, p.name) for p in params], ret, [], source='lambda')
        sig.cfunc = len(params) == 1 and bare_type(params[0].type)[0] == 'lua_State' and ret == 'int'
        return 'free', sig

    # ── lambda return type inference ─────────────────────────────────────────

    def infer_return(self, body, env, member):
        env = dict(env)
        types = []
        k = 0
        n = len(body)
        stmt_start = 0
        while k < n:
            t = body[k].t
            if t in ('(', '['):
                k = skip_balanced(body, k)
                continue
            if t == '{':
                # nested block: returns inside count, nested lambdas do not
                if k > 0 and body[k - 1].t in (')', ']', 'mutable') and self._is_lambda_block(body, k):
                    k = skip_balanced(body, k)
                    continue
                k += 1
                stmt_start = k
                continue
            if t in ('}', ';'):
                self._declare(body[stmt_start:k], env)
                k += 1
                stmt_start = k
                continue
            if t == 'return':
                e = k + 1
                while e < n and body[e].t != ';':
                    if body[e].t in _OPEN:
                        e = skip_balanced(body, e)
                        continue
                    e += 1
                expr = body[k + 1:e]
                types.append(self.expr_type(expr, env, member) if expr else '')
                k = e + 1
                stmt_start = k
                continue
            k += 1
        if not types:
            return ''
        known = [t for t in types if t]
        if not known:
            return '' if all(t == '' for t in types) else None
        for t in known:
            if t is not None:
                return t
        return None

    def _is_lambda_block(self, body, k):
        # walk back over `(params)` / `[captures]` to see a capture list
        j = k - 1
        if body[j].t == 'mutable':
            j -= 1
        if body[j].t == ')':
            depth = 0
            while j >= 0:
                if body[j].t == ')':
                    depth += 1
                elif body[j].t == '(':
                    depth -= 1
                    if depth == 0:
                        break
                j -= 1
            j -= 1
        return j >= 0 and body[j].t == ']'

    def _declare(self, stmt, env):
        """`Type name = ...` / `Type name;` local declarations feed the environment."""
        if len(stmt) >= 2 and stmt[0].k == 'id' and stmt[0].t not in ('return', 'if', 'else', 'for', 'while'):
            k = 0
            while k < len(stmt) and stmt[k].t not in ('=', '(', '{', ';'):
                if stmt[k].t == '<' and is_template_open(stmt, k):
                    k = skip_angles(stmt, k)
                    continue
                k += 1
            head = stmt[:k]
            if len(head) >= 2 and head[-1].k == 'id':
                tt = normalize_type(join_tokens(head[:-1]))
                if tt and tt != 'return':
                    env[head[-1].t] = tt if tt != 'auto' else self.expr_type(stmt[k + 1:], env, None) or 'auto'

    def expr_type(self, toks, env, member):
        toks = list(toks)
        if not toks:
            return None
        # strip outer parentheses
        while toks and toks[0].t == '(' and skip_balanced(toks, 0) == len(toks):
            inner = toks[1:-1]
            if self._looks_like_cast(inner) and False:
                break
            toks = inner
        if not toks:
            return None
        top = self._top_level_ops(toks)
        if '?' in top:
            q = top['?']
            c = top.get(':')
            branches = []
            if c is not None and c > q:
                branches = [toks[q + 1:c], toks[c + 1:]]
            for br in branches:
                bt = self.expr_type(br, env, member)
                if bt:
                    return bt
            return None
        for op in ('||', '&&', '==', '!=', '<=', '>=', '<', '>'):
            if op in top:
                return 'bool'
        if '+' in top or '-' in top or '*' in top or '/' in top:
            parts = []
            for op in ('+', '-', '*', '/'):
                if op in top:
                    pass
            lhs_types = []
            for part in self._split_ops(toks, ('+', '-', '*', '/')):
                lhs_types.append(self.expr_type(part, env, member))
            if any(t and bare_type(t)[0] in ('std::string', 'string', 'char') and bare_type(t)[1] <= 1 for t in lhs_types if t):
                return 'std::string'
            if any(t and bare_type(t)[0] in ('float', 'double') for t in lhs_types if t):
                return 'double'
            if all(t and bare_type(t)[0] in ('int', 'unsigned', 'long', 'size_t', 'uint32_t', 'int32_t') for t in lhs_types):
                return 'int'
            return None
        t0 = toks[0]
        if t0.t == '!':
            return 'bool'
        if len(toks) == 1:
            if t0.k == 'str':
                return 'std::string'
            if t0.k == 'chr':
                return 'char'
            if t0.k == 'num':
                return 'double' if re.search(r'[.eE]', t0.t) and not t0.t.startswith('0x') else 'int'
            if t0.t in ('true', 'false'):
                return 'bool'
            if t0.t == 'nullptr':
                return 'nullptr'
        # casts
        if t0.t in ('static_cast', 'reinterpret_cast', 'dynamic_cast', 'const_cast') and len(toks) > 1 and toks[1].t == '<':
            e = skip_angles(toks, 1)
            return normalize_type(join_tokens(toks[2:e - 1]))
        if t0.t == '(':
            e = skip_balanced(toks, 0)
            inner = toks[1:e - 1]
            if e < len(toks) and self._looks_like_cast(inner):
                return normalize_type(join_tokens(inner))
        if t0.t == '&':
            inner = self.expr_type(toks[1:], env, member)
            return inner + '*' if inner else None
        if t0.t == '*':
            inner = self.expr_type(toks[1:], env, member)
            return inner[:-1] if inner and inner.endswith('*') else None
        return self.postfix_type(toks, env, member)

    def _looks_like_cast(self, toks):
        if not toks:
            return False
        if all(x.k == 'id' or x.t in ('::', '*', '&', '<', '>', ',') for x in toks):
            name = bare_type(join_tokens(toks))[0]
            if name in ('size_t', 'int', 'float', 'double', 'bool', 'unsigned', 'uint32_t', 'int32_t', 'lua_Integer',
                        'unsigned int', 'long', 'uint64_t', 'int64_t'):
                return True
            if self.model.find_class(name) or self.model.find_enum(name) or name in self.model.typedefs:
                return True
        return False

    def _top_level_ops(self, toks):
        out = {}
        k = 0
        while k < len(toks):
            x = toks[k]
            if x.t in _OPEN:
                k = skip_balanced(toks, k)
                continue
            if x.t == '<' and is_template_open(toks, k):
                k = skip_angles(toks, k)
                continue
            if x.k == 'op' and x.t not in out:
                # unary +/-/*/& at the start or after an operator are not binary
                if x.t in ('+', '-', '*', '&') and (k == 0 or toks[k - 1].k == 'op' and toks[k - 1].t not in (')', ']')):
                    k += 1
                    continue
                out[x.t] = k
            k += 1
        return out

    def _split_ops(self, toks, ops):
        parts = []
        cur = []
        k = 0
        while k < len(toks):
            x = toks[k]
            if x.t in _OPEN:
                e = skip_balanced(toks, k)
                cur.extend(toks[k:e])
                k = e
                continue
            if x.t == '<' and is_template_open(toks, k):
                e = skip_angles(toks, k)
                cur.extend(toks[k:e])
                k = e
                continue
            if x.t in ops and cur:
                parts.append(cur)
                cur = []
            else:
                cur.append(x)
            k += 1
        parts.append(cur)
        return [p for p in parts if p]

    def postfix_type(self, toks, env, member):
        """Type of `primary (.name | ->name | ::name | <targs> | (args))*`."""
        k = 0
        n = len(toks)
        cur = None      # current type
        qual = []       # a qualified name being built (A::B::c)
        # primary
        x = toks[0]
        if x.k != 'id':
            return None
        names = [x.t]
        k = 1
        targs = None
        while k < n and toks[k].t == '::' and k + 1 < n and toks[k + 1].k == 'id':
            names.append(toks[k + 1].t)
            k += 2
        if k < n and toks[k].t == '<' and is_template_open(toks, k):
            e = skip_angles(toks, k)
            targs = split_top(toks[k + 1:e - 1])
            k = e
        call_args = None
        if k < n and toks[k].t == '(':
            e = skip_balanced(toks, k)
            call_args = split_top(toks[k + 1:e - 1])
            k = e
        if len(names) == 1 and names[0] in env and call_args is None:
            cur = env[names[0]]
        elif len(names) == 1 and names[0] == 'std' and False:
            pass
        elif names[0] == 'std' and len(names) >= 2:
            nm = names[-1]
            if nm in ('to_string', 'string', 'to_wstring'):
                cur = 'std::string'
            elif nm in ('min', 'max', 'abs', 'clamp') and call_args:
                cur = self.expr_type(call_args[0], env, member)
            else:
                return None
        elif len(names) >= 2:
            cls = '::'.join(names[:-1])
            nm = names[-1]
            if call_args is not None:
                fns = self.model.find_methods(cls, nm)
                fn = self.pick_by_arity(fns, len(call_args))
                if fn is None:
                    return None
                cur = self.instantiate_ret(fn, targs)
            else:
                fld = self.model.find_field(cls, nm)
                if fld is not None:
                    cur = fld.type
                else:
                    return None
        elif call_args is not None:
            nm = names[0]
            if self.model.find_class(nm) is not None or nm in ('std::string',):
                cur = nm  # functional cast / construction
            else:
                fns = self.model.functions.get(nm, [])
                fn = self.pick_by_arity(fns, len(call_args))
                if fn is None:
                    return None
                cur = self.instantiate_ret(fn, targs)
        else:
            return None
        # postfix chain
        while k < n and cur:
            x = toks[k]
            if x.t in ('.', '->') and k + 1 < n and toks[k + 1].k == 'id':
                nm = toks[k + 1].t
                k += 2
                targs = None
                if k < n and toks[k].t == '<' and is_template_open(toks, k):
                    e = skip_angles(toks, k)
                    targs = split_top(toks[k + 1:e - 1])
                    k = e
                base, stars = bare_type(cur)
                if k < n and toks[k].t == '(':
                    e = skip_balanced(toks, k)
                    args = split_top(toks[k + 1:e - 1])
                    k = e
                    m = re.match(r'(?:std::)?shared_ptr<(.+)>$', base)
                    if m and nm == 'get':
                        cur = m.group(1) + '*'
                        continue
                    if base in ('std::string', 'string') and nm in ('c_str', 'data'):
                        cur = 'const char*'
                        continue
                    if base in ('std::string', 'string') and nm in ('size', 'length'):
                        cur = 'size_t'
                        continue
                    fns = self.model.find_methods(base, nm)
                    fn = self.pick_by_arity(fns, len(args))
                    if fn is None:
                        return None
                    cur = self.instantiate_ret(fn, targs, base)
                else:
                    fld = self.model.find_field(base, nm)
                    if fld is None:
                        return None
                    cur = fld.type
                continue
            if x.t == '[':
                return None
            break
        if k < n:
            return None
        return cur

    def pick_by_arity(self, fns, nargs):
        cands = [f for f in fns if not f.ctor]
        exact = [f for f in cands if len(f.params) == nargs]
        if exact:
            return exact[0]
        ok = [f for f in cands if sum(1 for p in f.params if p.default is None) <= nargs <= len(f.params)]
        return ok[0] if ok else None

    def instantiate_ret(self, fn, targs, owner_type=None):
        ret = fn.ret
        if fn.template and fn.tparams and targs:
            subst = {pn: normalize_type(join_tokens(ta)) for pn, ta in zip(fn.tparams, targs)}
            ret = substitute(ret, subst)
        # methods of class templates (FunctionSubscribe<R(A...)>) are not specialized here
        return ret


def substitute(t, subst):
    if not t:
        return t
    for k, v in subst.items():
        t = re.sub(r'\b' + re.escape(k) + r'\b', v, t)
    return normalize_type(t)


# ═══════════════════════════════════════════════════════════════════════════════
# C++ -> Lua types
# ═══════════════════════════════════════════════════════════════════════════════

_INT_TYPES = {'int', 'unsigned', 'unsigned int', 'signed', 'signed int', 'short', 'unsigned short',
              'short int', 'long', 'unsigned long', 'long int', 'long long', 'unsigned long long',
              'long long int', 'size_t', 'std::size_t', 'ssize_t', 'ptrdiff_t', 'uint8_t', 'uint16_t',
              'uint32_t', 'uint64_t', 'int8_t', 'int16_t', 'int32_t', 'int64_t', 'lua_Integer',
              'unsigned char', 'signed char', 'std::byte', 'uintptr_t', 'intptr_t', 'std::uint32_t',
              'std::int32_t', 'std::uint64_t', 'std::int64_t', 'std::uint8_t', 'std::uint16_t'}
_NUM_TYPES = {'float', 'double', 'long double', 'lua_Number'}
_STR_TYPES = {'std::string', 'string', 'std::string_view', 'std::wstring', 'wstring', 'char', 'wchar_t',
              'char16_t', 'char32_t'}
_SEQ = re.compile(r'^(?:std::)?(?:vector|list|deque|set|unordered_set|multiset)<(.+)>$')
_ARR = re.compile(r'^(?:std::)?array<(.+),\s*[^,]+>$')
_MAP = re.compile(r'^(?:std::)?(?:map|unordered_map|multimap)<(.+)>$')
_OPT = re.compile(r'^(?:std::)?optional<(.+)>$')
_FUN = re.compile(r'^(?:std::)?function<(.+)>$')
_SHARED = re.compile(r'^(?:std::)?(?:shared_ptr|unique_ptr|weak_ptr)<(.+)>$')
_PAIR = re.compile(r'^(?:std::)?(?:pair|tuple)<(.+)>$')


class TypeMapper:
    def __init__(self, model, resolver):
        self.model = model
        self.r = resolver
        self.unknown = set()

    def lua(self, cpp, ctx_class=None, depth=0):
        """Lua annotation type of a C++ type ('' for void, 'any' when unknown)."""
        if cpp is None:
            return 'any'
        if cpp == '':
            return ''
        if depth > 10:
            return 'any'
        base, stars = bare_type(cpp)
        if base == 'void':
            return 'lightuserdata' if stars else ''
        if base in ('char', 'wchar_t') and stars >= 1:
            return 'string'
        if base == 'lua_State':
            return 'any'
        if base in ('LuaRef', 'luabridge::LuaRef'):
            return 'any'
        if base in ('LuaNil', 'nullptr', 'std::nullptr_t'):
            return 'nil'
        if base == 'bool':
            return 'boolean'
        if base in _NUM_TYPES:
            return 'number'
        if base in _INT_TYPES or base == 'Entity':
            return 'integer'
        if base in _STR_TYPES:
            return 'string'
        if base == 'Signature':
            return 'string'
        if base == 'Touch':
            return 'Touch'
        m = _OPT.match(base)
        if m:
            inner = self.lua(m.group(1), ctx_class, depth + 1)
            return f'{inner}|nil' if inner and inner != 'any' else 'any'
        m = _SEQ.match(base) or _ARR.match(base)
        if m:
            inner = self.lua(m.group(1), ctx_class, depth + 1)
            if not inner or inner == 'any':
                return 'any[]'
            return f'({inner})[]' if '|' in inner else f'{inner}[]'
        m = _MAP.match(base)
        if m:
            kv = split_type_list(m.group(1))
            if len(kv) == 2:
                return f'table<{self.lua(kv[0], ctx_class, depth + 1) or "any"}, {self.lua(kv[1], ctx_class, depth + 1) or "any"}>'
            return 'table'
        m = _PAIR.match(base)
        if m:
            return 'table'
        m = _FUN.match(base)
        if m:
            return self.fun_type(m.group(1), ctx_class, depth)
        m = _SHARED.match(base)
        if m:
            return self.lua(m.group(1), ctx_class, depth + 1)
        # bound classes (FunctionSubscribe<...> included)
        lua_name = self.r.cpp_to_lua.get(type_key(base))
        if lua_name:
            return lua_name
        # enums
        en = self.model.find_enum(base)
        if en is not None:
            return self.r.enum_ns.get(en.name, 'integer')
        if base in self.r.enum_ns:
            return self.r.enum_ns[base]
        # typedefs
        td = self.model.resolve_typedef(base, ctx_class)
        if td:
            return self.lua(td + '*' * stars, ctx_class, depth + 1)
        self.unknown.add(base)
        return 'any'

    def fun_type(self, sig, ctx_class, depth):
        m = re.match(r'^(.*?)\((.*)\)$', sig.strip())
        if not m:
            return 'function'
        ret = self.lua(m.group(1).strip(), ctx_class, depth + 1)
        args = [a for a in split_type_list(m.group(2)) if a and a != 'void']
        parts = [f'arg{i + 1}: {self.lua(a, ctx_class, depth + 1) or "any"}' for i, a in enumerate(args)]
        out = f'fun({", ".join(parts)})'
        if ret:
            out += f': {ret}'
        return out


def split_type_list(s):
    out = []
    depth = 0
    cur = ''
    for ch in s:
        if ch in '<([':
            depth += 1
        elif ch in '>)]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out
