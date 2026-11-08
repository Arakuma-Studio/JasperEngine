# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The one parser of the engine's Lua API (S23 R29): the LuaBridge bindings
(engine/core/script/binding/*.cpp) and the engine headers (engine/core) they bind, tokenized and
resolved to Lua types. extract.py turns the result into lua-api.json, which every emitter reads
(LuaLS stubs, the editor's completion header, the Markdown reference).

Moved from generate_lua_stubs.py (its tokenizer, C++ model, binding chains, type resolver and the
OVERRIDES of what C++ signatures cannot say), unchanged in behaviour.
"""

import os
import re

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
        self.macros = {}        # function-like #define: name -> ([params], body text)
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
                    elif name == 'define':
                        fm = re.match(r'(\w+)\(([^)]*)\)\s*(.*)', arg, re.S)
                        if fm:
                            params = [p.strip() for p in fm.group(2).split(',') if p.strip()]
                            self.macros[fm.group(1)] = (params, fm.group(3))
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
    __slots__ = ('name', 'ret', 'params', 'static', 'const', 'ctor', 'template', 'tparams', 'doc')

    def __init__(self, name, ret, params, static=False, const=False, ctor=False, template=False,
                 tparams=None, doc=None):
        self.name = name
        self.ret = ret
        self.params = params
        self.static = static
        self.const = const
        self.ctor = ctor
        self.template = template
        self.tparams = tparams or []
        self.doc = doc or []

    def __repr__(self):
        return f'{self.ret} {self.name}({", ".join(map(repr, self.params))})'


class CppField:
    __slots__ = ('type', 'doc')

    def __init__(self, type_, doc):
        self.type = type_
        self.doc = doc


class CppEnum:
    def __init__(self, name, values, doc):
        self.name = name
        self.values = values  # list of (name, int value or None, doc)
        self.doc = doc


class CppClass:
    def __init__(self, name, qual, doc):
        self.name = name
        self.qual = qual
        self.doc = doc
        self.bases = []
        self.methods = {}
        self.fields = {}
        self.typedefs = {}

    def __repr__(self):
        return f'CppClass({self.qual})'


_SPECIFIERS = {'virtual', 'static', 'inline', 'constexpr', 'explicit', 'mutable', 'extern',
               'consteval', 'constinit', 'thread_local', 'register'}
_TYPE_KEYWORDS = {'int', 'float', 'double', 'char', 'bool', 'void', 'long', 'short', 'unsigned',
                  'signed', 'const', 'volatile', 'auto', 'wchar_t', 'char8_t', 'char16_t',
                  'char32_t', 'struct', 'class', 'enum', 'typename'}
_RE_MACRO = re.compile(r'^[A-Z][A-Z0-9]*_[A-Z0-9_]+$')


class CppModel:
    def __init__(self):
        self.classes = {}    # simple name and Outer::Inner -> [CppClass]
        self.enums = {}      # simple name and Outer::Inner -> CppEnum
        self.typedefs = {}   # name -> type text
        self.functions = {}  # free functions: name -> [CppFunc]

    # ── parsing ──────────────────────────────────────────────────────────────

    def parse_file(self, path):
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            src = Source(path, f.read())
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
                while j < end and toks[j].t not in ('{', ';', '='):
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
                    # `typename T`, `class T = X`, `const T& Value`, `size_t N`
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
                r = self._try_class(src, toks, i, end, outer, start)
                if r is not None:
                    i = r
                    continue
            if t == 'enum':
                r = self._try_enum(src, toks, i, end, outer, start)
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
            decl, nxt = self._read_decl(toks, i, end)
            if nxt <= i:
                nxt = i + 1
            if decl:
                doc = src.doc_before(toks[start].line) + src.doc_trailing(decl[-1].line)
                if cls is None or access_public:
                    try:
                        self._interpret_decl(decl, cls, doc, template, tparams)
                    except Exception:
                        pass
            i = nxt

    def _try_class(self, src, toks, i, end, outer, start):
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
        cls = CppClass(name, qual, doc)
        cls.bases = bases
        cls._default_public = kind != 'class'
        self._register_class(cls, outer)
        self._parse_scope(src, toks, j + 1, k - 1, cls, outer + [name])
        # `struct {...} name;` variable declarations after the body are ignored
        while k < end and toks[k].t != ';':
            k += 1
        return k + 1

    def _try_enum(self, src, toks, i, end, outer, start):
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
        """One declaration: (tokens without body, next index)."""
        start = i
        saw_eq = False
        saw_paren = False
        while i < end:
            t = toks[i].t
            if t == ';':
                return toks[start:i], i + 1
            if t == '(' or t == '[':
                if t == '(' and not saw_eq:
                    saw_paren = True
                i = skip_balanced(toks, i)
                continue
            if t == '{':
                if saw_eq or not saw_paren:
                    i = skip_balanced(toks, i)
                    continue
                j = skip_balanced(toks, i)
                nxt = j + 1 if j < end and toks[j].t == ';' else j
                return toks[start:i], nxt
            if t == '}':
                return toks[start:i], i + 1
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
                    return toks[start:i], j
                continue
            i += 1
        return toks[start:end], end

    def _interpret_decl(self, decl, cls, doc, template, tparams):
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
            self._interpret_fields(toks, cls, doc)

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
        params = parse_params(toks[paren + 1:close - 1])
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
                     tparams=tparams, doc=doc)
        if cls is not None:
            cls.methods.setdefault(name, []).append(fn)
        else:
            self.functions.setdefault(name, []).append(fn)

    def _interpret_fields(self, toks, cls, doc):
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
            cls.fields[name] = CppField(ftype, doc)

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

    def resolve_typedef(self, name, cls_name=None):
        """Underlying type of a typedef name, or None."""
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
    if not toks or (len(toks) == 1 and toks[0].t == 'void'):
        return params
    for part in split_top(toks):
        if not part:
            continue
        if part[-1].t == '...':
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
    return params


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


# the engine's namespaces (jasperengine, its particles module) and LuaBridge's: bound C++ names are matched without them
_STD_STRIP = re.compile(r'\b(?:jasperengine|particles|luabridge)::')


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


def signature_key(t):
    """Exact key of a type in a function signature: only a top-level const of a by-value type is
    dropped (it is not part of the signature); references, pointers and other consts count."""
    t = normalize_type(t).replace('std::', '')
    if '&' not in t and '*' not in t:
        t = re.sub(r'\bconst\b', '', t)
    return t.replace(' ', '')


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
    __slots__ = ('name', 'targs', 'args', 'cond', 'doc', 'src', 'line')

    def __init__(self, name, targs, args, cond, doc, src, line=0):
        self.name = name
        self.targs = targs
        self.args = args
        self.cond = cond
        self.doc = doc
        self.src = src
        self.line = line


class LuaMember:
    def __init__(self, kind, name, call):
        self.kind = kind    # Method | StaticMethod | Property | StaticProperty
        self.name = name
        self.call = call
        self.writable = False
        self.copy = False
        self.doc = []
        self.unresolved = []


class LuaClass:
    def __init__(self, name, cpp, base_cpp, call):
        self.name = name
        self.cpp = cpp
        self.base_cpp = base_cpp
        self.base = None
        self.members = []
        self.ctors = []     # list of Sig
        self.doc = []
        self.cond = call.cond
        self.call = call


class LuaEnum:
    def __init__(self, name, call):
        self.name = name
        self.call = call
        self.values = []    # (name, value expr tokens, call)
        self.cpp_enum = None
        self.doc = []


class Sig:
    __slots__ = ('params', 'ret', 'doc', 'cfunc', 'problem', 'defaults')

    def __init__(self, params, ret, doc=None, cfunc=False, problem=None, defaults=None):
        self.params = params    # [(cpp type, name or None)], Lua-visible only
        self.ret = ret          # cpp type, '' for void, None for unknown
        self.doc = doc or []
        self.cfunc = cfunc
        self.problem = problem  # why the binding is wrong (a cast to a signature that does not exist)
        self.defaults = defaults  # the C++ default argument of each param (None: none), a declared function only

    def __repr__(self):
        return f'Sig({self.params} -> {self.ret!r})'


class Bindings:
    def __init__(self, model):
        self.model = model
        self.classes = []
        self.enums = []
        self.globals = []          # (name, value tokens, call)

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
        toks = src.tokens
        n = len(toks)
        i = 0
        while i < n:
            if toks[i].t == 'getGlobalNamespace' and i >= 2 and toks[i - 1].t == '::' and toks[i - 2].t == 'luabridge':
                chain_doc = src.doc_before(toks[i - 2].line)
                i = self._parse_chain(src, toks, i + 1, chain_doc)
                continue
            i += 1

    @staticmethod
    def _read_links(src, toks, i, calls, at=None):
        """The `.method<targs>(args)` links from toks[i]; returns where they stop. at: the macro
        invocation token the links were expanded from (their line, condition and doc)."""
        n = len(toks)
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
            where = at or name_tok
            doc = src.doc_before(where.line) if at is None or not calls or calls[-1].line != at.line else []
            calls.append(Call(name_tok.t, targs, args, where.cond, doc, src, where.line))
            i = k
        return i

    @staticmethod
    def _expand(src, toks, i):
        """A function-like macro of this file whose body is chain links (`#define P(a, b)
        .addProperty(a, ...)`), invoked at toks[i]: its body's tokens with the arguments put in,
        and where the invocation ends; None when toks[i] is not one."""
        macro = src.macros.get(toks[i].t) if toks[i].k == 'id' else None
        if macro is None or i + 1 >= len(toks) or toks[i + 1].t != '(':
            return None
        params, body = macro
        end = skip_balanced(toks, i + 1)
        args = split_top(toks[i + 2:end - 1])
        if len(args) != len(params):
            return None
        bound = dict(zip(params, args))
        out = []
        for tk in Source(src.path, body).tokens:
            out.extend(bound.get(tk.t, [tk]) if tk.k == 'id' else [tk])
        if not out or out[0].t != '.':
            return None
        return out, end

    def _parse_chain(self, src, toks, i, chain_doc):
        n = len(toks)
        i = skip_balanced(toks, i)  # (L)
        calls = []
        while i < n:
            i = self._read_links(src, toks, i, calls)
            # a macro that expands to links (CoreClassesLua.cpp's PBR_LUA_PROPERTY): its links, then on
            expanded = self._expand(src, toks, i) if i < n else None
            if expanded is None:
                break
            body, end = expanded
            self._read_links(src, body, 0, calls, at=toks[i])
            i = end
        ns = None
        cls = None
        for c in calls:
            if c.name == 'beginNamespace':
                ns = LuaEnum(literal(c.args[0]), c)
                ns.doc = chain_doc + c.doc
            elif c.name == 'endNamespace':
                if ns is not None and ns.values:
                    self.enums.append(ns)
                ns = None
            elif c.name in ('beginClass', 'deriveClass'):
                cpp = normalize_type(join_tokens(c.targs[0]))
                base = normalize_type(join_tokens(c.targs[1])) if c.name == 'deriveClass' else None
                cls = LuaClass(literal(c.args[0]), cpp, base, c)
                cls.doc = chain_doc + c.doc
            elif c.name == 'endClass':
                if cls is not None:
                    # LuaBridge re-opens a registered class (beginClass<T> again, e.g.
                    # HtmlUIVectorLua.cpp adds the <lottie> methods to UIElement): one class
                    reopened = next((k for k in self.classes if k.name == cls.name and k.cpp == cls.cpp), None)
                    if reopened is not None:
                        reopened.members.extend(cls.members)
                        reopened.ctors.extend(cls.ctors)
                        if not reopened.doc:
                            reopened.doc = cls.doc
                    else:
                        self.classes.append(cls)
                cls = None
            elif c.name == 'addVariable':
                if ns is not None:
                    ns.values.append((literal(c.args[0]), c.args[1], c))
                elif cls is None:
                    if chain_doc and not c.doc:
                        c.doc = chain_doc
                    self.globals.append((literal(c.args[0]), c.args[1], c))
            elif cls is not None:
                if c.name == 'addConstructor':
                    cls.ctors.append(c)
                elif c.name in ('addFunction', 'addStaticFunction', 'addProperty', 'addStaticProperty'):
                    kind = {'addFunction': 'Method', 'addStaticFunction': 'StaticMethod',
                            'addProperty': 'Property', 'addStaticProperty': 'StaticProperty'}[c.name]
                    cls.members.append(LuaMember(kind, literal(c.args[0]), c))
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
                return self.apply_cast(cast, toks[e:])
            return self.resolve(inner, member, hint)
        if t0 == 'static_cast' and len(toks) > 1 and toks[1].t == '<':
            e = skip_angles(toks, 1)
            cast = self.parse_fn_ptr_type(toks[2:e - 1])
            inner = toks[e + 1:skip_balanced(toks, e) - 1]
            if cast is not None:
                return self.apply_cast(cast, inner)
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
                params = parse_params(toks[e + 1:pe - 1])
                const = any(y.t == 'const' for y in toks[pe:])
                ret = normalize_type(join_tokens(toks[:k]))
                return ret, cls, [p.type for p in params], const
        return None

    def apply_cast(self, cast, target):
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
        # A cast to a function pointer type selects the overload with exactly that signature;
        # when there is none and the name has a single overload, the cast still compiles, as a
        # reinterpretation: LuaBridge then calls the function with arguments of the wrong
        # types (Vector3's __div was cast to an operator/(const Vector3&) that does not exist)
        problem = None
        if fns and not any(len(fn.params) == len(ptypes)
                           and all(signature_key(a.type) == signature_key(b) for a, b in zip(fn.params, ptypes))
                           and (cls is None or fn.static or fn.const == const)
                           and (fn.ret is None or signature_key(fn.ret) == signature_key(ret))
                           for fn in fns):
            have = '; '.join(f'{fn.ret} {name}({", ".join(p.type for p in fn.params)}){" const" if fn.const else ""}'
                             for fn in fns)
            problem = (f'cast to {ret} ({cls + "::" if cls else ""}*)({", ".join(ptypes)}){" const" if const else ""} '
                       f'matches no declaration of {name}: {have}')
        params = list(zip(ptypes, names))
        if cls is not None:
            return 'member', Sig(params, ret, doc, problem=problem)
        return 'free', Sig(params, ret, doc, problem=problem)

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
                sig = self.sig_of(fn)
                targs = self.template_args_of(toks)
                if fn.template and fn.tparams and targs:
                    subst = {pn: normalize_type(join_tokens(ta)) for pn, ta in zip(fn.tparams, targs)}
                    sig = Sig([(substitute(t, subst), n) for t, n in sig.params],
                              substitute(sig.ret, subst) if sig.ret else sig.ret, sig.doc, sig.cfunc, defaults=sig.defaults)
                return ('free' if fn.static else 'member'), sig
            fld = self.model.find_field(qual, name)
            if fld is not None:
                return 'data', Sig([], fld.type, fld.doc)
            return None
        # a free function or template of the binding file (or the engine)
        fns = self.local_functions(member.call.src.path and os.path.basename(member.call.src.path), name) \
            or self.model.functions.get(name, [])
        if not fns:
            return None
        fn = self.pick_overload(fns, hint)
        if fn is None:
            return None
        sig = self.sig_of(fn)
        targs = self.template_args_of(toks)
        if fn.template and fn.tparams and targs:
            subst = {}
            for pn, ta in zip(fn.tparams, targs):
                subst[pn] = normalize_type(join_tokens(ta))
            sig = Sig([(substitute(t, subst), n) for t, n in sig.params], substitute(sig.ret, subst) if sig.ret else sig.ret,
                      sig.doc, sig.cfunc)
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

    def sig_of(self, fn):
        params = [(p.type, p.name) for p in fn.params]
        cfunc = len(fn.params) == 1 and bare_type(fn.params[0].type)[0] == 'lua_State' and \
            re.sub(r'\s', '', fn.ret) in ('int',)
        return Sig(params, fn.ret, fn.doc, cfunc=cfunc, defaults=[p.default for p in fn.params])

    def resolve_lambda(self, toks, member):
        e = skip_balanced(toks, 0)  # captures
        k = e
        params = []
        if k < len(toks) and toks[k].t == '(':
            pe = skip_balanced(toks, k)
            params = parse_params(toks[k + 1:pe - 1])
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
        sig = Sig([(p.type, p.name) for p in params], ret)
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
        return known[0]

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
            toks = toks[1:-1]
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
                base = bare_type(cur)[0]
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
                    cur = self.instantiate_ret(fn, targs)
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

    def instantiate_ret(self, fn, targs):
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
_STR_TYPES = {'std::string', 'string', 'std::string_view', 'std::basic_string<char>', 'basic_string<char>', 'std::wstring', 'wstring', 'char', 'wchar_t',
              'char16_t', 'char32_t',
              # onPathFailed's reason: its snake_case name (luabridge::Stack<nav::PathFailure>, S76)
              'PathFailure', 'nav::PathFailure', 'jasperengine::nav::PathFailure',
              # asset path fields: plain path strings in Lua (luabridge::Stack<AssetRef>, LuaBridgeAddon.h)
              'AssetRef', 'ShaderRef', 'jasperengine::AssetRef', 'jasperengine::ShaderRef'}
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


# ═══════════════════════════════════════════════════════════════════════════════
# Hand-written signatures
# ═══════════════════════════════════════════════════════════════════════════════
# lua_CFunctions read their arguments from the Lua stack themselves, so their Lua
# signature cannot be derived from C++ types. Every lua_CFunction binding needs an entry
# here (the build lists the ones that do not have one as unresolved). Each overload is
# (params, returns): params are (name, Lua type) with a trailing '?' on optional names.

_OBJ = 'EntityHandle|ScriptInstance'
_ORIGINAL = 'PrefabAsset|string|integer|EntityHandle'

OVERRIDES = {
    ('GameObject', 'instantiate'): {
        'doc': [
            "Unity's Object.Instantiate. `original` is a PrefabAsset (a Prefab script property), a prefab",
            "path or name, a prefab id, any GameObject handle or a script instance (`self` of a script).",
            "A position goes with a rotation (use `Quaternion.IDENTITY`). Prefabs go to the active scene",
            "unless a parent or a scene is given. Returns the new root (named `<name>(Clone)`, its scripts",
            "already started); for a script instance, the copy's script instance of the same class.",
            "Returns nil when the prefab is not part of the build.",
        ],
        'overloads': [
            ([('original', _ORIGINAL)], 'GameObject'),
            ([('original', _ORIGINAL), ('parent', _OBJ), ('instantiateInWorldSpace?', 'boolean')], 'GameObject'),
            ([('original', _ORIGINAL), ('position', 'Vector3'), ('rotation', 'Quaternion'), ('parent?', _OBJ)], 'GameObject'),
            ([('original', 'PrefabAsset|string|integer'), ('scene', 'Scene'), ('position?', 'Vector3'), ('rotation?', 'Quaternion')], 'GameObject'),
            # a script instance gives the copy's script instance (typed `any`: LuaLS cannot name its class)
            ([('original', 'ScriptInstance')], 'any'),
            ([('original', 'ScriptInstance'), ('parent', _OBJ), ('instantiateInWorldSpace?', 'boolean')], 'any'),
            ([('original', 'ScriptInstance'), ('position', 'Vector3'), ('rotation', 'Quaternion'), ('parent?', _OBJ)], 'any'),
        ],
    },
    ('GameObject', 'destroy'): {
        'doc': ["Destroys the GameObject (and its children) at the end of the frame, or `delay` seconds",
                "later, calling Lua `onDestroy` first. `object` is any GameObject handle or a script instance;",
                "nil is ignored."],
        'overloads': [([('object', _OBJ + '|nil'), ('delay?', 'number')], '')],
    },
    ('GameObject', 'destroyImmediate'): {
        'doc': ["Destroys the GameObject (and its children) now. `object` is any GameObject handle or a",
                "script instance; nil is ignored."],
        'overloads': [([('object', _OBJ + '|nil')], '')],
    },
    ('Ease', 'setFunction'): {
        'doc': ['Uses `fn` as the easing function: it maps the time fraction (0..1) to the eased value.'],
        'overloads': [([('fn', 'fun(time: number): number')], '')],
    },
    ('LayerMask', 'getMask'): {
        'doc': ['Layer mask (an integer, as Unity) of the named layers: `LayerMask.getMask("Default", "Enemies")`',
                'or `LayerMask.getMask({"Default", "Enemies"})`.'],
        'overloads': [([('...', 'string')], 'integer'), ([('names', 'string[]')], 'integer')],
    },
}

OVERRIDES[('Screen', 'setExtraInsets')] = {
    'doc': ['Space the game keeps free per edge on top of the device insets (a banner ad): every safe-area',
            'consumer includes it (Screen.safeArea, the Safe Area component, HTML env()). `unit` is',
            '"canvas" (default) or "pixels" (view pixels, converted again when the canvas changes).'],
    'overloads': [([('left', 'number'), ('top', 'number'), ('right', 'number'), ('bottom', 'number'),
                    ('unit?', '"canvas"|"pixels"')], '')],
}

# script-lifecycle: the getScript family on every GameObject handle (and script instance, Script.lua),
# GameObject's scene-wide finds and the Event class (lua_CFunctions)
_GET_SCRIPT_DOCS = {
    'getScript': ["The Lua script instance of class `className` on this GameObject, enabled or not (Unity's",
                  "GetComponent<Script>()), or nil. Type it with `---@type Enemy` (LuaLS cannot infer the class)."],
    'getScripts': ["Every Lua script instance of this GameObject, or those of class `className`, in component order."],
    'getScriptInChildren': ["The first script of class `className` on this GameObject or a descendant (pre-order); inactive",
                            "GameObjects only with `includeInactive`. Scripts may be disabled (Unity's GetComponentInChildren)."],
    'getScriptsInChildren': ["Every script of class `className` on this GameObject and its descendants (pre-order); inactive",
                             "GameObjects only with `includeInactive`."],
    'getScriptInParent': ["The first script of class `className` on this GameObject or an ancestor (upward); inactive",
                          "GameObjects only with `includeInactive`."],
}
_GET_SCRIPT_SIGS = {
    'getScript': [([('className', 'string')], 'any')],
    'getScripts': [([('className?', 'string')], 'ScriptInstance[]')],
    'getScriptInChildren': [([('className', 'string'), ('includeInactive?', 'boolean')], 'any')],
    'getScriptsInChildren': [([('className', 'string'), ('includeInactive?', 'boolean')], 'ScriptInstance[]')],
    'getScriptInParent': [([('className', 'string'), ('includeInactive?', 'boolean')], 'any')],
}
for _name, _doc in _GET_SCRIPT_DOCS.items():
    OVERRIDES[('EntityHandle', _name)] = {'doc': _doc, 'overloads': _GET_SCRIPT_SIGS[_name]}
OVERRIDES.update({
    ('GameObject', 'find'): {
        'doc': ["The first active GameObject named `name` in the running scenes (main scene first, then the layers;",
                "ascending ids within a scene), else nil. `\"Arm/Hand\"` is a Hand below an Arm, `\"/Monster/Arm/Hand\"`",
                "a path from a root. Unity's GameObject.Find: not for every frame (a linear scan)."],
        'overloads': [([('name', 'string')], 'GameObject')]},
    ('GameObject', 'findWithTag'): {
        'doc': ["The first active GameObject with the tag in the running scenes, else nil. A tag the project",
                "does not define logs \"Tag: X is not defined.\" and finds nothing."],
        'overloads': [([('tag', 'string')], 'GameObject')]},
    ('GameObject', 'findGameObjectsWithTag'): {
        'doc': ["Every active GameObject with the tag in the running scenes (possibly empty)."],
        'overloads': [([('tag', 'string')], 'GameObject[]')]},
    ('GameObject', 'findObjectOfType'): {
        'doc': ["The first script instance of Lua class `className` that is active and enabled (any, with",
                "`includeInactive`) in the running scenes, else nil. A component wrapper name (\"Camera\",",
                "\"AudioSource\", \"Rigidbody\", ...) gives that wrapper for a GameObject with the component.",
                "Unity's FindObjectOfType: not for every frame (a linear scan)."],
        'overloads': [([('className', 'string'), ('includeInactive?', 'boolean')], 'any')]},
    ('GameObject', 'findObjectsOfType'): {
        'doc': ["Every match of findObjectOfType, in scene order (possibly empty)."],
        'overloads': [([('className', 'string'), ('includeInactive?', 'boolean')], 'any[]')]},
    ('Event', 'addListener'): {
        'doc': ["Adds a listener (Unity's AddListener / C# `+=`): a function, or a method of a script instance",
                "(`ev:addListener(self, \"onScore\")`), which is dropped once that script is destroyed. Adding the",
                "same one again does nothing."],
        'overloads': [([('fn', 'function')], ''), ([('owner', 'table'), ('methodName', 'string')], '')]},
    ('Event', 'removeListener'): {
        'doc': ["Removes a listener added with the same arguments (C# `-=`); true if one was removed."],
        'overloads': [([('fn', 'function')], 'boolean'), ([('owner', 'table'), ('methodName', 'string')], 'boolean')]},
    ('Event', 'removeAllListeners'): {
        'doc': ["Removes every listener."],
        'overloads': [([], '')]},
    ('Event', 'invoke'): {
        'doc': ["Calls the listeners in the order they were added with these arguments. A listener's error is",
                "logged with its traceback and the others still run; one added meanwhile runs from the next invoke."],
        'overloads': [([('...', 'any')], '')]},
    ('Event', 'add'): {
        'doc': ["Subscribes `fn` under `tag` (the event object protocol RegisterEvent uses); replaces that tag's."],
        'overloads': [([('tag', 'string'), ('fn', 'function')], 'boolean')]},
    ('Event', 'remove'): {
        'doc': ["Removes the subscription with `tag`; true if there was one."],
        'overloads': [([('tag', 'string')], 'boolean')]},
    ('Event', 'has'): {
        'doc': ["Whether a subscription with `tag` exists."],
        'overloads': [([('tag', 'string')], 'boolean')]},
    ('Event', '__tostring'): {
        'doc': [],
        'overloads': [([], 'string')]},
    # persistence (DontDestroyOnLoad)
    ('GameObject', 'dontDestroyOnLoad'): {
        'doc': ["Unity's DontDestroyOnLoad (also the global `DontDestroyOnLoad`): the root GameObject of `obj` (a",
                "script instance or a GameObject), with its children and running state, moves to the persistent",
                "scene and survives every SceneManager.loadScene. False (warning) for a GameObject with a parent."],
        'overloads': [([('obj', 'EntityHandle|ScriptInstance')], 'boolean')]},
    ('GameObject', 'isPersistent'): {
        'doc': ["In the persistent scene, or marked to move there (DontDestroyOnLoad)."],
        'overloads': [([('obj', 'EntityHandle|ScriptInstance')], 'boolean')]},
    ('GameObject', 'persistSingleton'): {
        'doc': ["The surviving instance of `self`'s class: another one already persistent is returned (and `self`'s",
                "GameObject destroyed at the end of the frame), else `self` is made persistent and returned.",
                "In init: `if GameObject.persistSingleton(self) ~= self then return end`."],
        'overloads': [([('self', 'ScriptInstance')], 'any')]},
    ('SceneManager', 'moveGameObjectToScene'): {
        'doc': ["Unity's MoveGameObjectToScene: a root GameObject goes to a running scene or the persistent scene",
                "(at the next safe point, with its running state). False (logged) when the scene is not loaded."],
        'overloads': [([('obj', 'EntityHandle|ScriptInstance'), ('scene', 'Scene')], 'boolean')]},
})

# Line and Trail Renderers (lines and trails spec 6.6): the points by 0-based index (Unity's
# SetPosition / GetPosition); an index out of range raises an error naming positionCount.
# setPosition(x, y[, z]) is still the GameObject's own position
for _renderer, _what in (('LineRenderer', 'polyline point'), ('TrailRenderer', 'stored trail point (0 = the oldest)')):
    OVERRIDES[(_renderer, 'setPosition')] = {
        'doc': [f'Sets the {_what} at `index` (0-based, as Unity\'s SetPosition); an index out of range is an',
                'error naming positionCount. With numbers only, (x, y[, z]) moves the GameObject itself.'],
        'overloads': [([('index', 'integer'), ('position', 'Vector3')], ''),
                      ([('x', 'number'), ('y', 'number'), ('z?', 'number')], '')]}
    OVERRIDES[(_renderer, 'getPosition')] = {
        'doc': [f'The {_what} at `index` (0-based); an index out of range is an error naming positionCount.'],
        'overloads': [([('index', 'integer')], 'Vector3')]}

# Particle Systems of version 2 (particles spec 6.6, docs/mcp/guide/particles.md)
OVERRIDES[('ParticleSystemModule', 'setBursts')] = {
    'doc': ['ps.emission:setBursts({{time = 0, count = 10, cycles = 1, interval = 0.01, probability = 1}, ...}): replaces',
            'the bursts (count a number or a MinMaxCurve; cycles 0 repeats forever). Version 2 systems only.'],
    'overloads': [([('bursts', '{time: number, count: MinMaxCurve|number, cycles?: integer, interval?: number, probability?: number}[]')], '')]}
OVERRIDES[('ParticleSystemModule', 'getBurst')] = {
    'doc': ['ps.emission:getBurst(index): the burst at `index` (0-based, as Unity\'s GetBurst) as a table.'],
    'overloads': [([('index', 'integer')], '{time: number, count: MinMaxCurve, cycles: integer, interval: number, probability: number}')]}
OVERRIDES[('ParticleSystemModule', 'setBurst')] = {
    'doc': ['ps.emission:setBurst(index, burst): sets the burst at `index` (0-based; burstCount appends).'],
    'overloads': [([('index', 'integer'), ('burst', '{time?: number, count?: MinMaxCurve|number, cycles?: integer, interval?: number, probability?: number}')], '')]}
OVERRIDES[('ParticleSystem', 'upgrade')] = {
    'doc': ['Converts a version 1 (legacy) system to the module model (version 2) and returns the report:',
            '`{upgraded = true, items = {{field, status = "exact"|"approximated"|"dropped", message, maxError}, ...}}`.',
            'A position modifier has no module equivalent: nothing changes unless `force` is true.'],
    'overloads': [([], '{upgraded: boolean, items: {field: string, status: string, message: string, maxError: number}[]}'),
                  ([('force', 'boolean')], '{upgraded: boolean, items: {field: string, status: string, message: string, maxError: number}[]}')]}

OVERRIDES[('Gradient', 'setKeys')] = {
    'doc': ['Replaces the keys: colorKeys are {time, color} (Vector3 sRGB) or GradientColorKey, alphaKeys',
            '{time, alpha} or GradientAlphaKey; sorted, at most 8 of each.',
            '`g:setKeys({{0, Vector3(1, 0.6, 0)}, {1, Vector3(1, 0.6, 0)}}, {{0, 1}, {1, 0}})`'],
    'overloads': [([('colorKeys', '({[1]: number, [2]: Vector3}|GradientColorKey)[]'),
                    ('alphaKeys?', '({[1]: number, [2]: number}|GradientAlphaKey)[]')], '')]}

# Input (CoreClassesLua.cpp): the rebinding functions read and push tables, the simulated
# sources take a name or a script instance
_INPUT_SOURCE = 'string|ScriptInstance'
OVERRIDES.update({
    ('Input', 'getBindings'): {
        'doc': ['The bindings the action uses now (a runtime override, else Project Settings > Input).'],
        'overloads': [([('name', 'string')], 'InputBinding[]')]},
    ('Input', 'setBindings'): {
        'doc': ['Replaces the action\'s bindings at run time (a settings screen); `false, error` when invalid.',
                'Input.resetBindings undoes it, Input.exportBindings saves it.'],
        'overloads': [([('name', 'string'), ('bindings', 'InputBinding[]')], 'boolean, string?')]},
    ('Input', 'importBindings'): {
        'doc': ['Replaces the runtime bindings with what Input.exportBindings returned; `false, error` when invalid.'],
        'overloads': [([('json', 'string')], 'boolean, string?')]},
    ('Input', 'simulateButton'): {
        'doc': ['Holds (or releases) the action\'s button like an on-screen button: a source keyed by `source`',
                '(default "script"; `self` = gone with the script instance). Seen by getButton* next frame.'],
        'overloads': [([('name', 'string'), ('pressed', 'boolean'), ('source?', _INPUT_SOURCE)], '')]},
    ('Input', 'simulateAxis'): {
        'doc': ['Adds `value` (-1..1) to the action like an on-screen stick, until set again; 0 removes it.',
                '`source` as in simulateButton.'],
        'overloads': [([('name', 'string'), ('value', 'number'), ('source?', _INPUT_SOURCE)], '')]},
    ('Input', 'addAxisDelta'): {
        'doc': ['Adds `delta` to the action for this frame only, unclamped (like mouse movement).'],
        'overloads': [([('name', 'string'), ('delta', 'number'), ('source?', _INPUT_SOURCE)], '')]},
    # ui-components: the selection (Unity's EventSystem.currentSelectedGameObject)
    ('UI', 'getSelected'): {
        'doc': ['The Button selected in `scene` (keyboard, gamepad and TV menus), nil for none.'],
        'overloads': [([('scene', 'Scene')], 'GameObject?')]},
    ('UI', 'setSelected'): {
        'doc': ['Selects a Button of `scene` (Unity\'s SetSelectedGameObject); nil deselects. False when it',
                'cannot be selected (not a Button, inactive, hidden, an on-screen control).'],
        'overloads': [([('scene', 'Scene'), ('button', 'GameObject|integer|nil')], 'boolean')]},
    ('Button', 'findSelectableOnUp'): {
        'doc': ['Where the selection moves up from this Button (its Navigation), nil for nowhere.'],
        'overloads': [([], 'GameObject?')]},
    ('Button', 'findSelectableOnDown'): {
        'doc': ['Where the selection moves down from this Button (its Navigation), nil for nowhere.'],
        'overloads': [([], 'GameObject?')]},
    ('Button', 'findSelectableOnLeft'): {
        'doc': ['Where the selection moves left from this Button (its Navigation), nil for nowhere.'],
        'overloads': [([], 'GameObject?')]},
    ('Button', 'findSelectableOnRight'): {
        'doc': ['Where the selection moves right from this Button (its Navigation), nil for nowhere.'],
        'overloads': [([], 'GameObject?')]},
    ('UIComponent', 'getPointerEventData'): {
        'doc': ['The PointerEventData of the legacy callback running now (`onPointerDown(x, y)`, `onClick`...):',
                'its pointerId, button, positions; nil outside one.'],
        'overloads': [([], 'PointerEventData?')]},
    ('Input', 'isPointerOverUI'): {
        'doc': ['Whether the pointer is over UI that takes it (Unity\'s EventSystem.IsPointerOverGameObject):',
                'a component UI element that receives events, HTML content, or a press either holds.',
                '`pointerId`: -1 the mouse, a finger\'s `Touch.fingerId`; none: the mouse or any finger.'],
        'overloads': [([('pointerId?', 'integer')], 'boolean')]},
    ('Input', 'clearSimulated'): {
        'doc': ['Removes the simulated values of `source` (without one: of every script).'],
        'overloads': [([('source?', _INPUT_SOURCE)], '')]},
})

# Shader (CoreClassesLua.cpp): global shader values (Unity's Shader.SetGlobal* / PropertyToID),
# read by shaders declaring u_vs_globals / u_fs_globals members (or a texture uniform) of the name
_GLOBAL = 'string|integer'
OVERRIDES.update({
    ('Shader', 'propertyToID'): {
        'doc': ['A stable id of the global `name` for this run (Unity\'s Shader.PropertyToID): pass it instead of the name.'],
        'overloads': [([('name', 'string')], 'integer')]},
    ('Shader', 'setGlobalFloat'): {
        'doc': ['Sets a float global (a vector global takes (value, 0, 0, 0)); false when it is a matrix, array or texture.'],
        'overloads': [([('name', _GLOBAL), ('value', 'number')], 'boolean')]},
    ('Shader', 'setGlobalInt'): {
        'doc': ['As setGlobalFloat: the integer is stored as a float (Unity\'s SetGlobalInt).'],
        'overloads': [([('name', _GLOBAL), ('value', 'integer')], 'boolean')]},
    ('Shader', 'setGlobalVector'): {
        'doc': ['Sets a vector global (missing components 0; a float global takes x).'],
        'overloads': [([('name', _GLOBAL), ('value', 'Vector4|Vector3|Vector2')], 'boolean')]},
    ('Shader', 'setGlobalColor'): {
        'doc': ['Sets a colour global from sRGB (stored linear, alpha as is; a Vector3 is opaque).'],
        'overloads': [([('name', _GLOBAL), ('color', 'Vector4|Vector3')], 'boolean')]},
    ('Shader', 'setGlobalMatrix'): {
        'doc': ['Sets a matrix4 global (only a matrix global takes it).'],
        'overloads': [([('name', _GLOBAL), ('value', 'Matrix4')], 'boolean')]},
    ('Shader', 'setGlobalFloatArray'): {
        'doc': ['Sets the elements of a float array global (1..64; a first set fixes the size, longer ones are truncated).'],
        'overloads': [([('name', _GLOBAL), ('values', 'number[]')], 'boolean')]},
    ('Shader', 'setGlobalVectorArray'): {
        'doc': ['Sets the elements of a vector4 array global (1..64; a first set fixes the size).'],
        'overloads': [([('name', _GLOBAL), ('values', '(Vector4|Vector3|Vector2)[]')], 'boolean')]},
    ('Shader', 'setGlobalTexture'): {
        'doc': ['Sets a texture global: a Texture, an image path or a Framebuffer (a render target); nil restores its default.'],
        'overloads': [([('name', _GLOBAL), ('texture', 'Texture|string|Framebuffer|nil')], 'boolean')]},
    ('Shader', 'getGlobalFloat'): {
        'doc': ['The global\'s value (a vector\'s x); nil when it does not exist or is a matrix or texture.'],
        'overloads': [([('name', _GLOBAL)], 'number?')]},
    ('Shader', 'getGlobalInt'): {
        'doc': ['getGlobalFloat as an integer.'],
        'overloads': [([('name', _GLOBAL)], 'integer?')]},
    ('Shader', 'getGlobalVector'): {
        'doc': ['The global\'s value (an array\'s element 0); nil when it does not exist or is a matrix or texture.'],
        'overloads': [([('name', _GLOBAL)], 'Vector4?')]},
    ('Shader', 'getGlobalColor'): {
        'doc': ['getGlobalVector as sRGB (alpha as is).'],
        'overloads': [([('name', _GLOBAL)], 'Vector4?')]},
    ('Shader', 'getGlobalMatrix'): {
        'doc': ['A matrix4 global\'s value; nil for any other.'],
        'overloads': [([('name', _GLOBAL)], 'Matrix4?')]},
    ('Shader', 'getGlobalFloatArray'): {
        'doc': ['An array global\'s elements (x of each); nil when it is not an array.'],
        'overloads': [([('name', _GLOBAL)], 'number[]?')]},
    ('Shader', 'getGlobalVectorArray'): {
        'doc': ['An array global\'s elements; nil when it is not an array.'],
        'overloads': [([('name', _GLOBAL)], 'Vector4[]?')]},
    ('Shader', 'getGlobalTexture'): {
        'doc': ['The texture a texture global holds; nil for none (its fallback binds) or another kind.'],
        'overloads': [([('name', _GLOBAL)], 'Texture?')]},
    ('Shader', 'hasGlobal'): {
        'doc': ['Whether the global exists (declared in Project Settings, or set by a script).'],
        'overloads': [([('name', _GLOBAL)], 'boolean')]},
    ('Shader', 'getGlobalNames'): {
        'doc': ['The globals that exist: the declared ones first, then the ones scripts made.'],
        'overloads': [([], 'string[]')]},
})
for _owner in ('Material', 'Image', 'Text', 'Polygon', 'Points', 'LineRenderer', 'Skybox', 'TrailRenderer'):
    OVERRIDES[(_owner, 'setShaderGlobalBinding')] = {
        'doc': ['Makes the shader parameter `member` follow the global shader value `global` (Shader.setGlobal*),',
                'over its own value; nil unbinds. False when nothing changed.' + (' Then mesh:setMaterial(m).' if _owner == 'Material' else '')],
        'overloads': [([('member', 'string'), ('global', 'string|nil')], 'boolean')]}
    OVERRIDES[(_owner, 'getShaderGlobalBinding')] = {
        'doc': ['The global `member` follows, nil when it has its own value.'],
        'overloads': [([('member', 'string')], 'string?')]}
OVERRIDES[('Scene', 'setPostProcessGlobalBinding')] = {
    'doc': ['Makes the u_fs_postParams member of pass `index` follow a global shader value; nil unbinds.'],
    'overloads': [([('index', 'integer'), ('member', 'string'), ('global', 'string|nil')], 'boolean')]}
OVERRIDES[('Scene', 'getPostProcessGlobalBinding')] = {
    'doc': ['The global the pass member follows, nil when none.'],
    'overloads': [([('index', 'integer'), ('member', 'string')], 'string?')]}
# Canvas Group fades (ui canvas group spec 7.6): AlphaTweens on the group from its current alpha
OVERRIDES[('CanvasGroup', 'fadeTo')] = {
    'doc': ['Fades the group from its current alpha to `alpha` in `duration` seconds (LeanTween.alphaCanvas,',
            'DOTween DOFade); a new fade stops the previous one. Returns the running AlphaTween.',
            '`group:fadeTo(0, 0.25, {deactivate = true, onComplete = function() ... end})`'],
    'overloads': [([('alpha', 'number'), ('duration', 'number'), ('options?', 'CanvasGroupFadeOptions')], 'AlphaTween')]}
OVERRIDES[('CanvasGroup', 'fadeIn')] = {
    'doc': ['fadeTo(1, duration, options).'],
    'overloads': [([('duration', 'number'), ('options?', 'CanvasGroupFadeOptions')], 'AlphaTween')]}
OVERRIDES[('CanvasGroup', 'fadeOut')] = {
    'doc': ['fadeTo(0, duration, options); with `deactivate = true` the GameObject is deactivated at the end.'],
    'overloads': [([('duration', 'number'), ('options?', 'CanvasGroupFadeOptions')], 'AlphaTween')]}
OVERRIDES[('CanvasGroup', 'stopFade')] = {
    'doc': ['Stops the running fade; `jumpToEnd` writes its end alpha and completes it now (onComplete, deactivate).'],
    'overloads': [([('jumpToEnd?', 'boolean')], '')]}

# <particle-system>: nil before its prefab is made or when its src failed (a LuaRef)
OVERRIDES[('UIElement', 'getParticleSystem')] = {
    'doc': ['The Particle System of a <particle-system> element\'s hidden GameObject; nil before the prefab',
            'is made (document scripts, onLoad, edit mode) or when its src failed.'],
    'overloads': [([], 'ParticleSystem|nil')]}

# Mesh:getMaterial and Mesh.material push the Material copy themselves (a LuaRef: the copy
# remembers the shader its submesh draws with, LuaBinding::setMaterialDrawnShader)
OVERRIDES[('Mesh', 'getMaterial')] = {
    'doc': ['A copy of the submesh\'s material (submesh 0 by default): edit it, then `setMaterial` it back.'],
    'overloads': [([], 'Material'), ([('submesh', 'integer')], 'Material')],
}

# S13: dynamic resolution as the renderer applies it (a table built in the binding)
OVERRIDES[('Scene', 'getDynamicResolution')] = {
    'doc': ['Dynamic resolution as the renderer applies it: `scale` (the render scale drawn at), `source`',
            '("gpu", "frame" or "none": the law of the last measurement), `smoothedMs` / `targetMs`, and',
            '`enabled`, `running`, `idle` (why not), `overridden`, `minScale`, `maxScale`, `saturatedLow`, `ineffective`,',
            '`acceptedChanges`, `probeSeconds`.'],
    'overloads': [([], '{enabled: boolean, running: boolean, idle: string, overridden: boolean, scale: number, minScale: number, '
                       'maxScale: number, source: string, smoothedMs: number, targetMs: number, saturatedLow: boolean, '
                       'ineffective: boolean, acceptedChanges: integer, probeSeconds: number}')],
}
OVERRIDES[('Scene', 'setDynamicResolutionOverride')] = {
    'doc': ['Pins the main camera\'s render scale (0.5..1) over dynamic resolution while the game runs; nil releases it.'],
    'overloads': [([('scale', 'number|nil')], '')],
}

# script-data (IOClassesLua.cpp): Json, File helpers and PlayerPrefs are lua_CFunctions (option
# tables, `nil, err` results); JsonEncodeOptions / JsonDecodeOptions / JsonNull / JsonValue are
# in engine/core/script/binding/stubs/Json.lua
_JSON_VALUE = 'JsonValue'
OVERRIDES.update({
    ('Json', 'encode'): {
        'doc': ['JSON text of `value` (tables, strings, numbers, booleans, Json.null, Vector2/3/4, Quaternion,',
                'a `__tojson` metamethod). Keys sorted; a table with keys 1..n is an array, an empty one `[]`',
                '(Json.object({}) or emptyTable = "object" for `{}`); integers stay integers, floats keep `.0`.',
                'Raises with the place: "Json.encode: cannot encode a function at $.player.onHit".'],
        'overloads': [([('value', 'any'), ('opts?', 'JsonEncodeOptions')], 'string')]},
    ('Json', 'decode'): {
        'doc': ['The Lua value of JSON `text`: objects and arrays become tables (empty ones marked, so they',
                'encode back the same), null becomes Json.null (or nil with `null = "nil"`), integers integers.',
                'Raises "Json.decode: syntax error at line 3, column 14: ...".'],
        'overloads': [([('text', 'string'), ('opts?', 'JsonDecodeOptions')], _JSON_VALUE)]},
    ('Json', 'tryEncode'): {
        'doc': ['Json.encode without raising: the text, or nil and the message (options are still checked).'],
        'overloads': [([('value', 'any'), ('opts?', 'JsonEncodeOptions')], 'string?, string?')]},
    ('Json', 'tryDecode'): {
        'doc': ['Json.decode without raising: the value, or nil and the message. A JSON `null` gives Json.null,',
                'so nil always means failure.'],
        'overloads': [([('text', 'string'), ('opts?', 'JsonDecodeOptions')], _JSON_VALUE + '?, string?')]},
    ('Json', 'isNull'): {
        'doc': ['`v` is Json.null.'],
        'overloads': [([('v', 'any')], 'boolean')]},
    ('Json', 'array'): {
        'doc': ['Marks `t` (or a new table) to encode as a JSON array (`[]` when empty; holes are an error); returns it.'],
        'overloads': [([('t?', 'table')], 'table')]},
    ('Json', 'object'): {
        'doc': ['Marks `t` (or a new table) to encode as a JSON object (`{}` when empty); returns it.'],
        'overloads': [([('t?', 'table')], 'table')]},
    ('Json', 'readFile'): {
        'doc': ['File.readText + Json.decode: the value, or nil and "not found: data://save.json" /',
                '"data://save.json: syntax error at line 2, column 5: ...". Paths as File.readText.'],
        'overloads': [([('path', 'string'), ('opts?', 'JsonDecodeOptions')], _JSON_VALUE + '?, string?')]},
    ('Json', 'writeFile'): {
        'doc': ['Json.encode + File.writeText (atomic): true, or nil and the message. Paths as File.writeText.'],
        'overloads': [([('path', 'string'), ('value', 'any'), ('opts?', 'JsonEncodeOptions')], 'true?, string?')]},
    ('File', 'readText'): {
        'doc': ['The whole file as a string, or nil and the message ("not found: data://save.json"). Paths:',
                '"data://x" (user data, writable), "asset://x" and "lua://x" (read-only), a bare relative path',
                'means data://; ".." is refused; absolute paths are read on desktop and in the editor only.'],
        'overloads': [([('path', 'string')], 'string?, string?')]},
    ('File', 'writeText'): {
        'doc': ['Replaces the data:// file with `text` atomically (missing folders made): true, or nil and',
                'the message. On the web it reaches IndexedDB within half a second (System.syncFileSystem: now).'],
        'overloads': [([('path', 'string'), ('text', 'string')], 'true?, string?')]},
    ('File', 'appendText'): {
        'doc': ['Adds `text` to the end of the data:// file (made when missing; not atomic): true, or nil and the message.'],
        'overloads': [([('path', 'string'), ('text', 'string')], 'true?, string?')]},
    ('File', 'exists'): {
        'doc': ['A file or folder is at `path` (paths as File.readText).'],
        'overloads': [([('path', 'string')], 'boolean')]},
    ('File', 'remove'): {
        'doc': ['Removes the data:// file (never a folder): true (also when nothing was there), or nil and the message.'],
        'overloads': [([('path', 'string')], 'true?, string?')]},
    ('File', 'makeDirs'): {
        'doc': ['Makes the data:// folder and the missing ones above it: true, or nil and the message.'],
        'overloads': [([('path', 'string')], 'true?, string?')]},
    ('File', 'list'): {
        'doc': ['Names of the files and folders in a data:// folder, sorted, or nil and the message.'],
        'overloads': [([('dir', 'string')], 'string[]?, string?')]},
    ('PlayerPrefs', 'getInt'): {
        'doc': ["Unity's PlayerPrefs.GetInt: the stored integer, or `default` (0) when the key is not there."],
        'overloads': [([('key', 'string'), ('default?', 'integer')], 'integer')]},
    ('PlayerPrefs', 'setInt'): {
        'doc': ['Stores a 32-bit integer (3.5 raises "integer expected").'],
        'overloads': [([('key', 'string'), ('value', 'integer')], '')]},
    ('PlayerPrefs', 'getFloat'): {
        'doc': ['The stored float, or `default` (0.0) when the key is not there.'],
        'overloads': [([('key', 'string'), ('default?', 'number')], 'number')]},
    ('PlayerPrefs', 'setFloat'): {
        'doc': ['Stores a float (32-bit, as Unity).'],
        'overloads': [([('key', 'string'), ('value', 'number')], '')]},
    ('PlayerPrefs', 'getString'): {
        'doc': ['The stored string (an empty one too), or `default` ("") when the key is not there.'],
        'overloads': [([('key', 'string'), ('default?', 'string')], 'string')]},
    ('PlayerPrefs', 'hasKey'): {
        'doc': ['The key is stored (an empty value too).'],
        'overloads': [([('key', 'string')], 'boolean')]},
    ('PlayerPrefs', 'deleteKey'): {
        'doc': ['Removes the key; nothing happens when it is not there.'],
        'overloads': [([('key', 'string')], '')]},
    ('PlayerPrefs', 'setString'): {
        'doc': ['Stores a string (Json.encode a table to store it).'],
        'overloads': [([('key', 'string'), ('value', 'string')], '')]},
})

# coroutines (binding/CoroutineClassesLua.cpp, lua_CFunctions of ScriptScheduler.cpp): the wait
# helpers yield the running engine coroutine ('async': LuaLS ---@async, so calling one outside a
# coroutine is flagged); TimerOptions is in engine/core/script/binding/stubs/Timer.lua
_AWAITABLE = 'Coroutine|Timer|Action|AudioSource|AnimationPlayer|AnimeTimer|{isDone: fun(self): boolean}'
OVERRIDES.update({
    ('Coroutine', 'start'): {
        'doc': ['Runs `fn(...)` in a new engine coroutine owned by no script (it survives scene loads and ends at',
                'Play stop): for required modules and HTML UI document scripts; scripts use self:startCoroutine. The',
                'code up to the first wait runs now. Errors are logged with the coroutine\'s traceback.'],
        'overloads': [([('fn', 'async fun(...)'), ('...', 'any')], 'Coroutine')]},
    ('Coroutine', 'running'): {
        'doc': ['The engine coroutine running now, or nil (in onUpdate and other event handlers).'],
        'overloads': [([], 'Coroutine?')]},
    ('Coroutine', 'isInside'): {
        'doc': ['True inside an engine coroutine (where the wait helpers work).'],
        'overloads': [([], 'boolean')]},
    ('Coroutine', 'wait'): {
        'async': True,
        'doc': ["Unity's WaitForSeconds: waits `seconds` of game time (scaled: never ends at Time.timeScale = 0;",
                '<= 0 = the next frame). Returns the game seconds that passed.'],
        'overloads': [([('seconds', 'number')], 'number')]},
    ('Coroutine', 'waitRealtime'): {
        'async': True,
        'doc': ["Unity's WaitForSecondsRealtime: waits unscaled seconds (runs at time scale 0, stops while the app or",
                "the editor's Play is paused). Returns the unscaled seconds that passed."],
        'overloads': [([('seconds', 'number')], 'number')]},
    ('Coroutine', 'nextFrame'): {
        'async': True,
        'doc': ["Unity's `yield return null`: resumes in the next frame, after onUpdate (as coroutine.yield())."],
        'overloads': [([], '')]},
    ('Coroutine', 'waitFrames'): {
        'async': True,
        'doc': ['Waits `n` (>= 1) frames.'],
        'overloads': [([('n', 'integer')], '')]},
    ('Coroutine', 'waitForFixedUpdate'): {
        'async': True,
        'doc': ["Unity's WaitForFixedUpdate: resumes after the next fixed step's physics (none at time scale 0)."],
        'overloads': [([], '')]},
    ('Coroutine', 'waitForEndOfFrame'): {
        'async': True,
        'doc': ["Unity's WaitForEndOfFrame: resumes after this frame's draw."],
        'overloads': [([], '')]},
    ('Coroutine', 'waitUntil'): {
        'async': True,
        'doc': ["Unity's WaitUntil: waits until `predicate()` is truthy (checked every frame after onUpdate; it must",
                'not wait). With `timeout` seconds (unscaled when `unscaled`): false when it ran out, else true.'],
        'overloads': [([('predicate', 'fun(): any'), ('timeout?', 'number'), ('unscaled?', 'boolean')], 'boolean')]},
    ('Coroutine', 'waitWhile'): {
        'async': True,
        'doc': ["Unity's WaitWhile: waits while `predicate()` is truthy; `timeout` as in waitUntil."],
        'overloads': [([('predicate', 'fun(): any'), ('timeout?', 'number'), ('unscaled?', 'boolean')], 'boolean')]},
    ('Coroutine', 'waitFor'): {
        'async': True,
        'doc': ["`yield return StartCoroutine(...)` and friends: waits until `x` is done. A Coroutine: true and what",
                'it returned, or false and its error ("stopped" when stopped; at once when it already ended); a Timer:',
                'true when it fired, false when cancelled (not an endless every); a Tween/Action, AudioSource or',
                'AnimationPlayer: true when stopped / not playing, false when its GameObject is gone; an anime',
                'animation or timer: true completed, false cancelled; anything with isDone() (an HTTP request): true, x.'],
        'overloads': [([('x', _AWAITABLE)], 'boolean, any')]},
    ('Timer', 'after'): {
        'doc': ['Calls `fn()` once after `delay` seconds (game time; `opts.unscaled` for real time), at the earliest',
                'the next frame. Owned by no script unless `opts.owner = self` (then cancelled with it); in a script',
                'self:invoke is shorter. For HTML UI animation timing anime.createTimer stays (milliseconds).'],
        'overloads': [([('delay', 'number'), ('fn', 'fun()'), ('opts?', 'TimerOptions')], 'Timer')]},
    ('Timer', 'every'): {
        'doc': ['Calls `fn(timer)` every `interval` seconds (first after `opts.delay`, default the interval), at most',
                'once a frame, `opts.count` times or until cancelled; `fn` returning false cancels it.'],
        'overloads': [([('interval', 'number'), ('fn', 'fun(timer: Timer): boolean?'), ('opts?', 'TimerOptions')], 'Timer')]},
})

# pixel fonts (GameObjectClassesLua.cpp, docs/specs/S51-pixel-fonts.md R29): load returns the font or
# nil and the reason; measure returns two integers
OVERRIDES.update({
    ('BitmapFont', 'load'): {
        'doc': ['A bitmap font (.bitmapfont, .fnt or "builtin:pixel") and its metrics, in whole font pixels;',
                '`nil, error` when it does not load.'],
        'overloads': [([('path', 'string')], 'BitmapFont?, string?')]},
    ('BitmapFont', 'measure'): {
        'doc': ['The width and height of the text (newlines split lines) in whole pixels at `scale` (default 1):',
                'the layout a Text draws.'],
        'overloads': [([('text', 'string'), ('scale?', 'integer')], 'integer, integer')]},
})

# audio-mixer (ECSClassesLua.cpp, GameObjectClassesLua.cpp): option tables ({unscaled = true}),
# `nil` results and table results; AudioTimeOptions / AudioFadeOutOptions are in
# engine/core/script/binding/stubs/Audio.lua
OVERRIDES.update({
    ('AudioMixer', 'setFloat'): {
        'doc': ["Unity's AudioMixer.SetFloat: sets an exposed parameter (a group volume in dB, an effect value),",
                'over `seconds` (default at once). It wins over snapshots until clearFloat. False (logged once)',
                'for an unknown name. Remembered parameters are saved to UserSettings at the end of the frame.'],
        'overloads': [([('name', 'string'), ('value', 'number'), ('seconds?', 'number'), ('opts?', 'AudioTimeOptions')], 'boolean')]},
    ('AudioMixer', 'getFloat'): {
        'doc': ['The value an exposed parameter has now (a script value, else the snapshot / mixer value); nil when unknown.'],
        'overloads': [([('name', 'string')], 'number?')]},
    ('AudioMixer', 'getParameterNames'): {
        'doc': ['The exposed parameter names.'],
        'overloads': [([], 'string[]')]},
    ('AudioMixer', 'transitionTo'): {
        'doc': ["Unity's AudioMixerSnapshot.TransitionTo: every parameter moves to the snapshot over `seconds`",
                '(dB linearly, frequencies along log2). False for an unknown snapshot.'],
        'overloads': [([('snapshot', 'string'), ('seconds', 'number'), ('opts?', 'AudioTimeOptions')], 'boolean')]},
    ('AudioMixer', 'transitionToSnapshots'): {
        'doc': ["Unity's AudioMixer.TransitionToSnapshots: to the weighted blend of snapshots (weights normalized)."],
        'overloads': [([('snapshots', 'string[]'), ('weights', 'number[]'), ('seconds', 'number'), ('opts?', 'AudioTimeOptions')], 'boolean')]},
    ('AudioMixer', 'getSnapshotNames'): {
        'doc': ['The snapshot names.'],
        'overloads': [([], 'string[]')]},
    ('AudioMixer', 'getGroup'): {
        'doc': ['A mixer group by name (live values; setting its volume is a script value, like setFloat), nil when none.'],
        'overloads': [([('name', 'string')], 'AudioMixerGroup?')]},
    ('AudioMixer', 'getGroupNames'): {
        'doc': ['The group names, Master first, parents before children.'],
        'overloads': [([], 'string[]')]},
    ('AudioMixerGroup', 'getEffect'): {
        'doc': ['An effect of the group by index (1-based) or type ("lowpass": the first of it); nil when none.'],
        'overloads': [([('index', 'integer')], 'AudioMixerEffect?'), ([('type', 'string')], 'AudioMixerEffect?')]},
    ('AudioMixerGroup', 'setVolume'): {
        'doc': ['Moves the group volume (dB) over `seconds` (a script value, like setFloat).'],
        'overloads': [([('db', 'number'), ('seconds?', 'number'), ('opts?', 'AudioTimeOptions')], '')]},
    ('AudioMixerEffect', 'setParameter'): {
        'doc': ['Sets a parameter of the effect ("cutoff", "resonance", "wet", ...: docs audio.md), over `seconds`.',
                'A script value (wins over snapshots). False when the effect has no such parameter.'],
        'overloads': [([('name', 'string'), ('value', 'number'), ('seconds?', 'number'), ('opts?', 'AudioTimeOptions')], 'boolean')]},
    ('AudioMixerEffect', 'getParameter'): {
        'doc': ['The value a parameter has now; nil when the effect has no such parameter.'],
        'overloads': [([('name', 'string')], 'number?')]},
    ('AudioSystem', 'preload'): {
        'doc': ['Decodes (or opens, for streaming) a clip ahead so its first play or one-shot has no load; held until',
                'the game ends. `loadType` as AudioSource.loadType (default Auto). False when the file does not load.'],
        'overloads': [([('path', 'string'), ('loadType?', 'AudioLoadType')], 'boolean')]},
    ('AudioSource', 'playOneShot'): {
        'doc': ["Unity's PlayOneShot: the clip, or a Sound Event's pick (its .soundevent path or GUID), as an extra",
                "voice over what plays (volume x volumeScale, the source's group, pitch, pan and position). Up to",
                'maxVoices voices; the oldest one-shot is stolen.'],
        'overloads': [([('path', 'string'), ('volumeScale?', 'number')], '')]},
    ('Audio', 'play'): {
        'doc': ['A Sound Event (GUID or .soundevent path) or a clip path as a free voice. Without target or position it is 2D.',
                'An unknown name gives an invalid handle and one warning per name per Play. Inside a physics callback it',
                'starts at the frame\'s audio update.'],
        'overloads': [([('eventOrClip', 'string'), ('opts?', 'AudioPlayOptions')], 'SoundHandle')]},
    ('Audio', 'stopAll'): {
        'doc': ['Stops the voices started by Audio.play, triggers and one-shots: all, or those routed to `group` or a group',
                'below it. The count stopped (0 inside a physics callback, where it runs at the audio update).',
                'AudioSystem.stopAll also stops the sources\' main voices.'],
        'overloads': [([('group?', 'string')], 'integer')]},
    ('Audio', 'mark'): {
        'doc': ['Writes a marker line at this sample in the audio capture of the Play or player run; does nothing without one.',
                'At most 200 bytes of label.'],
        'overloads': [([('label', 'string')], '')]},
    ('Audio', 'isCapturing'): {
        'doc': ['True while this Play or player run writes an audio capture.'],
        'overloads': [([], 'boolean')]},
    ('Audio', 'playMusic'): {
        'doc': ['Starts or switches the music: a clip (its loop region repeats until advanceMusic) or a Sound Event played as',
                'a playlist of sections. Survives scene loads. The playing track again does nothing. False, warned once,',
                'for a layer event or sections that differ in rate or channels.'],
        'overloads': [([('eventOrClip', 'string'), ('opts?', 'MusicOptions')], 'boolean')]},
    ('Audio', 'stopMusic'): {
        'doc': ['A linear fade to silence over `fade` seconds (default at once), then the music stops; opts.sync: when the fade starts.'],
        'overloads': [([('fade?', 'number'), ('opts?', 'MusicStopOptions')], '')]},
    ('Audio', 'advanceMusic'): {
        'doc': ['The current section ends its loop at its next loop end, then its tail plays and the next section follows.',
                'False without music.'],
        'overloads': [([], 'boolean')]},
    ('Audio', 'playStinger'): {
        'doc': ['A clip or Sound Event on the music\'s next "beat" (default), "bar" or "now", sample-exact; owned by the music,',
                'so a scene load does not cut it. Without music or tempo it plays now, warned once.'],
        'overloads': [([('eventOrClip', 'string'), ('sync?', 'string')], 'SoundHandle')]},
    ('Audio', 'getMusic'): {
        'doc': ['The track playing (the incoming one during a crossfade), "" without music.'],
        'overloads': [([], 'string')]},
    ('Audio', 'getMusicTime'): {
        'doc': ['Seconds of the track played so far, all sections.'],
        'overloads': [([], 'number')]},
    ('Audio', 'getMusicPosition'): {
        'doc': ['Where the music is on its beat grid (bar and beat 1-based, 0 before the first beat); nil without music or tempo.'],
        'overloads': [([], 'MusicPosition?')]},
    ('SoundHandle', 'fadeOut'): {
        'doc': ['To silence over `seconds`, then stops (opts.pause: pauses); the volume is put back.'],
        'overloads': [([('seconds', 'number'), ('opts?', 'AudioFadeOutOptions')], '')]},
    ('SoundHandle', 'fadeTo'): {
        'doc': ['The volume moves to `volume` over `seconds`.'],
        'overloads': [([('volume', 'number'), ('seconds', 'number'), ('opts?', 'AudioTimeOptions')], '')]},
    ('AudioSource', 'fadeIn'): {
        'doc': ['Plays from silence to the current volume over `seconds` (starting it when it is not playing).'],
        'overloads': [([('seconds', 'number'), ('opts?', 'AudioTimeOptions')], '')]},
    ('AudioSource', 'fadeOut'): {
        'doc': ['To silence over `seconds`, then stops (opts.pause: pauses); the volume is put back for the next play().'],
        'overloads': [([('seconds', 'number'), ('opts?', 'AudioFadeOutOptions')], '')]},
    ('AudioSource', 'fadeTo'): {
        'doc': ['The volume moves to `volume` over `seconds`.'],
        'overloads': [([('volume', 'number'), ('seconds', 'number'), ('opts?', 'AudioTimeOptions')], '')]},
    ('AudioSource', 'playClipAtPoint'): {
        'doc': ["Unity's AudioSource.PlayClipAtPoint: the clip at a world position (3D when the scene has a camera),",
                'through a mixer group (name; default Master); it belongs to `scene` and stops with it.'],
        'overloads': [([('scene', 'Scene'), ('path', 'string'), ('position', 'Vector3'), ('volume?', 'number'), ('group?', 'string')], '')]},
})

# http (NetClassesLua.cpp): the request functions are lua_CFunctions (positional options and
# callback, either may be left out); HttpOptions / HttpRequestOptions are in
# engine/core/script/binding/stubs/Http.lua
_HTTP_CB = 'fun(response: HttpResponse)'
_HTTP_OPTS = 'HttpOptions'
def _http_overloads(head):
    return [
        (head, 'HttpRequest'),
        (head + [('callback', _HTTP_CB)], 'HttpRequest'),
        (head + [('options', _HTTP_OPTS), ('callback?', _HTTP_CB)], 'HttpRequest'),
    ]
OVERRIDES.update({
    ('Http', 'get'): {
        'doc': ['GET `url` (UnityWebRequest.Get). The callback runs once on the main thread, before onUpdate, with',
                'the response: `if res.ok then ... else print(res.errorCode, res.error) end`. Pass `owner = self` in',
                'per-scene scripts: the request then ends (no callback) when the script goes.'],
        'overloads': _http_overloads([('url', 'string')])},
    ('Http', 'head'): {
        'doc': ['HEAD `url`: the status and headers, no body (a connectivity check).'],
        'overloads': _http_overloads([('url', 'string')])},
    ('Http', 'delete'): {
        'doc': ['DELETE `url`.'],
        'overloads': _http_overloads([('url', 'string')])},
    ('Http', 'post'): {
        'doc': ['POST `body` to `url`: a string is sent as is (Content-Type: application/octet-stream unless options.headers',
                'set one), a table as JSON (Content-Type: application/json), nil sends an empty body.'],
        'overloads': _http_overloads([('url', 'string'), ('body', 'string|table|nil')])},
    ('Http', 'put'): {
        'doc': ['PUT `body` to `url` (body rules of Http.post).'],
        'overloads': _http_overloads([('url', 'string'), ('body', 'string|table|nil')])},
    ('Http', 'patch'): {
        'doc': ['PATCH `body` to `url` (body rules of Http.post).'],
        'overloads': _http_overloads([('url', 'string'), ('body', 'string|table|nil')])},
    ('Http', 'request'): {
        'doc': ['The full form: options.url (required), options.method (default "GET"), options.onComplete and every',
                'other option.'],
        'overloads': [([('options', 'HttpRequestOptions')], 'HttpRequest')]},
    ('Http', 'download'): {
        'doc': ['GET `url` into the file `path` (data://ads/x.mp4, or an absolute path inside System.getUserDataPath();',
                'folders are made). Streamed to <path>.part and renamed on success: a failed or cancelled download',
                'leaves an existing file as it was. response.file is the path.'],
        'overloads': _http_overloads([('url', 'string'), ('path', 'string')])},
    ('Http', 'getTexture'): {
        'doc': ['GET an image (PNG, JPEG, BMP, TGA, GIF) and decode it off the main thread: response.textureData,',
                'for image:setTexture(response.url, response.textureData). Not an image: errorCode "decode".'],
        'overloads': _http_overloads([('url', 'string')])},
    ('Http', 'encodeUrl'): {
        'doc': ['Percent-encodes `text` (RFC 3986: letters, digits and -._~ stay): "a b&c" -> "a%20b%26c".'],
        'overloads': [([('text', 'string')], 'string')]},
    ('Http', 'decodeUrl'): {
        'doc': ['Decodes %XX escapes, and "+" as a space.'],
        'overloads': [([('text', 'string')], 'string')]},
    ('HttpResponse', 'json'): {
        'doc': ['Json.decode of the body: the value, or nil and the message.'],
        'overloads': [([], 'JsonValue?, string?')]},
    ('HttpResponse', 'header'): {
        'doc': ['The header `name` (any case), or nil.'],
        'overloads': [([('name', 'string')], 'string?')]},
})

# native-bridge (NativeClassesLua.cpp): the Native functions are lua_CFunctions (positional
# optional args, callback, options); NativeCallOptions / NativeErrorCode / NativePending are in
# engine/core/script/binding/stubs/Native.lua
_NATIVE_CB = 'fun(result: JsonValue, errMessage: string?, errCode: NativeErrorCode?)'
OVERRIDES.update({
    ('Native', 'call'): {
        'doc': ['Calls `method` of the native plugin `plugin` now (Unity\'s AndroidJavaClass.CallStatic): a C++ plugin,',
                'else the platform\'s (Java/Kotlin static method, Objective-C/Swift class method, JavaScript function),',
                'else a Lua mock. Arguments and the result are JSON values. Errors are values:',
                '`local v, err, code = Native.call("Device", "getCode")`.'],
        'overloads': [([('plugin', 'string'), ('method', 'string'), ('...', 'JsonValue')], 'JsonValue, string?, NativeErrorCode?')]},
    ('Native', 'callAsync'): {
        'doc': ['Starts an asynchronous call; `callback(result, errMessage, errCode)` runs once on the main thread, before',
                'onUpdate (not when cancelled or its owner went). Pass `{owner = self}` in per-scene scripts.'],
        'overloads': [
            ([('plugin', 'string'), ('method', 'string')], 'NativeCall'),
            ([('plugin', 'string'), ('method', 'string'), ('callback', _NATIVE_CB), ('options?', 'NativeCallOptions')], 'NativeCall'),
            ([('plugin', 'string'), ('method', 'string'), ('args', 'JsonValue[]|nil'), ('callback?', _NATIVE_CB), ('options?', 'NativeCallOptions')], 'NativeCall'),
        ]},
    ('Native', 'on'): {
        'doc': ['Subscribes to native events (JasperNative.emit on the native side): `handler(payload)` runs on the main thread',
                'before onUpdate, handlers in registration order. With an owner it goes with it. Returns the id for Native.off.'],
        'overloads': [([('plugin', 'string'), ('event', 'string'), ('handler', 'fun(payload: JsonValue)'), ('owner?', 'ScriptInstance|EntityHandle|Scene')], 'integer')]},
    ('Native', 'off'): {
        'doc': ['Removes a Native.on handler (an unknown id does nothing).'],
        'overloads': [([('id', 'integer')], '')]},
    ('Native', 'has'): {
        'doc': ['A C++, platform or mock implementation of the plugin exists here; with `method`, that method exists.'],
        'overloads': [([('plugin', 'string'), ('method?', 'string')], 'boolean')]},
    ('Native', 'setMock'): {
        'doc': ['A Lua mock of the plugin, used only when no C++ or platform plugin exists (editor Play, desktop): each',
                'function gets the call\'s arguments (async calls: plus the NativeCall last) and returns the value, or nil,',
                'message [, code], or Native.pending() to complete later with call:resolve / call:reject.'],
        'overloads': [([('plugin', 'string'), ('mock', 'table<string, function>')], '')]},
    ('Native', 'clearMock'): {
        'doc': ['Removes the Native.setMock mock of the plugin.'],
        'overloads': [([('plugin', 'string')], '')]},
    ('Native', 'emitMock'): {
        'doc': ['Fires the Native.on handlers of (plugin, event) in the next frame, as if native code emitted it.'],
        'overloads': [([('plugin', 'string'), ('event', 'string'), ('payload?', 'JsonValue')], '')]},
    ('Native', 'pending'): {
        'doc': ['What an async mock function returns to complete its call later (call:resolve / call:reject).'],
        'overloads': [([], 'NativePending')]},
    ('NativeCall', 'resolve'): {
        'doc': ['Mocks: completes the call with `value` (false when it was already completed or cancelled).'],
        'overloads': [([('value?', 'JsonValue')], 'boolean')]},
    ('NativeCall', 'reject'): {
        'doc': ['Mocks: fails the call with `code` and `message` (false when it was already completed or cancelled).'],
        'overloads': [([('code', 'NativeErrorCode|string'), ('message?', 'string')], 'boolean')]},
})

# Properties whose getter returns a LuaRef, or whose lambda's return type is not inferred:
# (Lua type, a copy)
PROPERTY_OVERRIDES = {
    # the null sentinel (stubs/Json.lua: JsonNull)
    ('Json', 'null'): ('JsonNull', False),
    ('Mesh', 'material'): ('Material', True),
    # the Particle System module views (binding/stubs/ParticleSystemModules.lua, from
    # tools/gen_particle_stubs.py): live views, not copies
    **{('ParticleSystem', _m): ('ParticleSystem' + _m[0].upper() + _m[1:] + 'Module', False) for _m in (
        'main', 'emission', 'shape', 'velocityOverLifetime', 'limitVelocityOverLifetime', 'inheritVelocity',
        'forceOverLifetime', 'colorOverLifetime', 'colorBySpeed', 'sizeOverLifetime', 'sizeBySpeed',
        'rotationOverLifetime', 'rotationBySpeed', 'noise', 'textureSheetAnimation', 'collision', 'trails',
        'customData', 'externalForces', 'renderer')},
    # AnimationEvent.args builds a 1-based table of the event's arguments
    ('AnimationEvent', 'args'): ('(number|string|boolean)[]', True),
    # ui-multitouch: the GameObjects of a PointerEventData (nil: none)
    ('PointerEventData', 'pointerEnter'): ('GameObject?', False),
    ('PointerEventData', 'pointerPress'): ('GameObject?', False),
    ('PointerEventData', 'rawPointerPress'): ('GameObject?', False),
    ('PointerEventData', 'pointerDrag'): ('GameObject?', False),
    ('PointerEventData', 'target'): ('GameObject?', False),
    # a coroutine's error (with its traceback) and the script instance that started it
    ('Coroutine', 'error'): ('string?', False),
    ('Coroutine', 'owner'): ('ScriptInstance?', False),
    # the GameObject a Camera Follow follows (its scene or the persistent one), nil without one
    ('CameraFollow', 'target'): ('GameObject?', False),
    ('HttpRequest', 'response'): ('HttpResponse|nil', False),
    ('NativeCall', 'result'): ('JsonValue', False),
    ('NativeCall', 'errorCode'): ('NativeErrorCode|nil', False),
    ('NativeCall', 'errorMessage'): ('string|nil', False),
    ('HttpResponse', 'headers'): ('table<string, string>', True),
    ('HttpResponse', 'body'): ('string', False),
    ('HttpResponse', 'data'): ('JsonValue', False),
    ('HttpResponse', 'textureData'): ('TextureData|nil', True),

    # the joint's bodies (std::optional<GameObject> lambdas): nil = the world
    ('Joint2D', 'bodyA'): ('GameObject|nil', False),
    ('Joint2D', 'bodyB'): ('GameObject|nil', False),
    ('Joint2D', 'connectedBody'): ('GameObject|nil', False),
    ('Joint3D', 'bodyA'): ('GameObject|nil', False),
    ('Joint3D', 'bodyB'): ('GameObject|nil', False),
    ('Joint3D', 'connectedBody'): ('GameObject|nil', False),
    # ui-components: a Button's Explicit navigation targets and the selection events' GameObjects
    ('Button', 'selectOnUp'): ('GameObject|nil', False),
    ('Button', 'selectOnDown'): ('GameObject|nil', False),
    ('Button', 'selectOnLeft'): ('GameObject|nil', False),
    ('Button', 'selectOnRight'): ('GameObject|nil', False),
    ('Button', 'onClick'): ('FunctionSubscribe', False),
    ('BaseEventData', 'target'): ('GameObject?', False),
    ('BaseEventData', 'selectedObject'): ('GameObject?', False),
}

# physics-contacts: the callbacks' values. Their GameObject / body / receiver getters are
# std::optional lambdas (nil once gone), contacts builds a table, getContact is a lua_CFunction
for _col, _body, _contact in (('Collider', 'Rigidbody', 'ContactPoint'), ('Collider2D', 'Rigidbody2D', 'ContactPoint2D')):
    PROPERTY_OVERRIDES[(_col, 'gameObject')] = ('GameObject|nil', False)
    PROPERTY_OVERRIDES[(_col, 'rigidbody')] = (_body + '|nil', False)
    PROPERTY_OVERRIDES[(_col, 'attachedRigidbody')] = (_body + '|nil', False)
    PROPERTY_OVERRIDES[(_col, 'receiver')] = ('GameObject|nil', False)
for _coll, _body, _contact in (('Collision', 'Rigidbody', 'ContactPoint'), ('Collision2D', 'Rigidbody2D', 'ContactPoint2D')):
    PROPERTY_OVERRIDES[(_coll, 'gameObject')] = ('GameObject|nil', False)
    PROPERTY_OVERRIDES[(_coll, 'rigidbody')] = (_body + '|nil', False)
    PROPERTY_OVERRIDES[(_coll, 'thisRigidbody')] = (_body + '|nil', False)
    PROPERTY_OVERRIDES[(_coll, 'contacts')] = (_contact + '[]', True)
    OVERRIDES[(_coll, 'getContact')] = {
        'doc': ['Contact point `index` (0-based, an error out of range). Allocates nothing, unlike `contacts`.'],
        'overloads': [([('index', 'integer')], _contact)]}
PROPERTY_OVERRIDES[('Collision2D', 'otherRigidbody')] = ('Rigidbody2D|nil', False)

for _phys, _what in (('Physics', 'Rigidbody'), ('Physics2D', 'Rigidbody 2D')):
    OVERRIDES[(_phys, 'ignoreCollision')] = {
        'doc': [f'Makes the two bodies (GameObjects or bodies with a {_what}, one scene) collide or not, at run',
                'time (Unity Physics.IgnoreCollision). `ignore` defaults to true; resting pairs are woken.'],
        'overloads': [([('a', 'EntityHandle'), ('b', 'EntityHandle'), ('ignore?', 'boolean')], '')]}
    OVERRIDES[(_phys, 'getIgnoreCollision')] = {
        'doc': ['Whether ignoreCollision keeps the two bodies apart.'],
        'overloads': [([('a', 'EntityHandle'), ('b', 'EntityHandle')], 'boolean')]}

for _phys, _vec in (('Physics', 'Vector3'), ('Physics2D', 'Vector2')):
    OVERRIDES[(_phys, 'ignoreLayerCollision')] = {
        'doc': ['Makes the two layers (0..31) collide or not (Layer Collision Matrix); `ignore` defaults to true.'],
        'overloads': [([('layer1', 'integer'), ('layer2', 'integer'), ('ignore?', 'boolean')], '')]}
    OVERRIDES[(_phys, 'getIgnoreLayerCollision')] = {
        'doc': ['Whether collisions between the two layers are ignored.'],
        'overloads': [([('layer1', 'integer'), ('layer2', 'integer')], 'boolean')]}
    OVERRIDES[(_phys, 'getLayerCollisionMask')] = {
        'doc': ['Mask of the layers `layer` collides with.'],
        'overloads': [([('layer', 'integer')], 'integer')]}
    OVERRIDES[(_phys, 'setLayerCollisionMask')] = {
        'doc': ['Sets the mask of the layers `layer` collides with.'],
        'overloads': [([('layer', 'integer'), ('mask', 'integer')], '')]}
    OVERRIDES[(_phys, 'resetLayerCollisionMatrix')] = {
        'doc': ['Every layer collides with every layer again.'],
        'overloads': [([], '')]}
    for _all in (False, True):
        _ret = 'RayReturn[]' if _all else 'RayReturn'
        OVERRIDES[(_phys, 'raycastAll' if _all else 'raycast')] = {
            'doc': [('Every hit' if _all else 'The closest hit') + f' of a ray in the scene\'s physics world ({_vec} origin and',
                    'direction, or a Ray), on the layers of `layerMask` (default: Physics.DEFAULT_RAYCAST_LAYERS).'],
            'overloads': [([('scene', 'Scene'), ('ray', 'Ray'), ('layerMask?', 'integer')], _ret),
                          ([('scene', 'Scene'), ('origin', _vec), ('direction', _vec), ('maxDistance', 'number'),
                            ('layerMask?', 'integer')], _ret)]}

# S26: MeshRaycast, the triangle-exact raycast without physics (Physics.raycast's arguments)
_MESH_RAYCAST_OPTIONS = '{hitBackfaces: boolean?, includeInactive: boolean?, ignore: (EntityHandle|integer)[]?, kinds: integer?}'
for _all in (False, True):
    _ret = 'MeshRaycastHit[]' if _all else 'MeshRaycastHit'
    OVERRIDES[('MeshRaycast', 'raycastAll' if _all else 'raycast')] = {
        'doc': [('Every hit, nearest first,' if _all else 'The closest hit') + ' of a ray against the scene\'s meshes, instances, terrains',
                'and UI rects (their triangles, no colliders). With a Ray, distance is the fraction of ray.direction; with',
                'origin, direction and maxDistance it is in world units. options: hitBackfaces, includeInactive, ignore,',
                'kinds (MeshRaycast.MESH | INSTANCE | TERRAIN | UI | BOUNDS). docs/scripting/mesh-raycast.md'],
        'overloads': [([('scene', 'Scene'), ('ray', 'Ray'), ('layerMask?', 'integer'), ('options?', _MESH_RAYCAST_OPTIONS)], _ret),
                      ([('scene', 'Scene'), ('origin', 'Vector3'), ('direction', 'Vector3'), ('maxDistance', 'number'),
                        ('layerMask?', 'integer'), ('options?', _MESH_RAYCAST_OPTIONS)], _ret)]}
OVERRIDES[('MeshRaycast', 'raycastEntity')] = {
    'doc': ['The closest hit of a ray on one GameObject only (distance: the fraction of ray.direction).'],
    'overloads': [([('scene', 'Scene'), ('entity', 'EntityHandle|integer'), ('ray', 'Ray'), ('options?', _MESH_RAYCAST_OPTIONS)],
                   'MeshRaycastHit')]}
OVERRIDES[('MeshRaycast', 'prepare')] = {
    'doc': ['Builds the triangle trees of these GameObjects\' meshes (all when none) in the background, so a later',
            'raycast does not build them inline.'],
    'overloads': [([('scene', 'Scene'), ('entities?', '(EntityHandle|integer)[]')], '')]}
# S22 input actions: the lua_CFunctions of InputActions (numbers or Vector2 values, an option
# table and a callback)
OVERRIDES[('InputActions', 'setVirtual')] = {
    'doc': ['Feeds the virtual control `virtual/<name>` (an on-screen stick or button of the HTML UI):',
            'a number (x) or a Vector2.'],
    'overloads': [([('name', 'string'), ('value', 'number|Vector2')], '')]}
OVERRIDES[('InputActions', 'inject')] = {
    'doc': ['Forces the action ("Map/Action") to `value` (a number or a Vector2) for `windows` frame windows',
            '(1 by default; 0 presses and releases in one window, -1 holds it until releaseInjected), at top',
            'priority.'],
    'overloads': [([('action', 'string'), ('value', 'number|Vector2'), ('windows?', 'integer')], '')]}
OVERRIDES[('InputActions', 'startRebind')] = {
    'doc': ['Waits for the next actuated control (a key, a mouse button, a gamepad button, an axis past 0.5)',
            'and makes it the binding\'s override; every map is suspended meanwhile. `options`: `groups`,',
            '`exclude` (path globs), `cancel` (a path, `keyboard/escape` by default; false for none),',
            '`timeout` (seconds). `onComplete` gets {status = "completed"|"canceled"|"timed_out", action,',
            'bindingId, path}. False when the action or the binding is not there.'],
    'overloads': [([('action', 'string'), ('bindingId', 'string'),
                    ('options?', '{groups?: string[], exclude?: string[], cancel?: string|false, timeout?: number}'),
                    ('onComplete?', 'fun(result: {status: string, action: string, bindingId: string, path: string})')],
                   'boolean')]}
OVERRIDES[('InputActions', 'getConflicts')] = {
    'doc': ['The bindings of enabled maps in the same group as the binding with its path.'],
    'overloads': [([('action', 'string'), ('bindingId', 'string')],
                   '{action: string, id: string, path: string, groups: string[]}[]')]}

# Camera shake (camera shake spec 8): the shake calls are lua_CFunctions (a preset is a name, a
# .shake path or a table; options a table); ShakePreset and ShakeOptions are in
# engine/core/script/binding/stubs/CameraShake.lua
_SHAKE_PRESET = 'ShakePresetName|string|ShakePreset'
for _cls, _who, _example in (
        ('CameraShake', 'every global Camera Shaker (enabled, active, Global on, in every running scene)',
         '`CameraShake.shake("explosion")`, `CameraShake.shake("assets/shakes/boom.shake", {strength = 0.5})`'),
        ('CameraShaker', 'this shaker, whatever its Global flag',
         '`self.gameObject:getCameraShaker():shake("punch", {seed = 7})`')):
    OVERRIDES[(_cls, 'shake')] = {
        'doc': [f'Starts a shake on {_who}.',
                '`preset`: a built-in name (CameraShake.getPresetNames()), a .shake path or a preset table. Returns',
                'the ShakeInstance (invalid when nothing was shaken; an unknown name logs one warning).',
                _example],
        'overloads': [([('preset', _SHAKE_PRESET), ('options?', 'ShakeOptions')], 'ShakeInstance')]}
    OVERRIDES[(_cls, 'shakeAt')] = {
        'doc': [f'As shake, on {_who}, with the strength and roughness scaled by',
                'max(0, 1 - d / radius), d the distance from the shaker (x/y only in 2D); none at d >= radius.'],
        'overloads': [([('position', 'Vector3'), ('radius', 'number'), ('preset', _SHAKE_PRESET),
                        ('options?', 'ShakeOptions')], 'ShakeInstance')]}

# HTML UI (phase 1, p1-lua): the element API's lua_CFunctions (they take Lua functions and
# option tables, and raise errors with no C++ object alive)
_LISTENER = 'fun(event: UIEvent)'
_LISTENER_OPTIONS = 'boolean|UIEventListenerOptions'
for _cls, _target in (('UIElement', 'the element'), ('UIDocument', 'the document element')):
    OVERRIDES[(_cls, 'addEventListener')] = {
        'doc': [f'Calls `listener` with a UIEvent when a `type` event reaches {_target} (browser names: click,',
                'keydown, input, change, wheel ...: UIEventType). `options` is `true` for the capture phase, or',
                '`{capture = boolean, once = boolean}`. The same function, type and phase again is ignored.',
                'A custom event of dispatchEvent: `---@cast name UIEventType` first (any name works at run time).'],
        'overloads': [([('type', 'UIEventType'), ('listener', _LISTENER), ('options?', _LISTENER_OPTIONS)], '')]}
    OVERRIDES[(_cls, 'removeEventListener')] = {
        'doc': ['Removes the listener added with the same type, function and capture phase.'],
        'overloads': [([('type', 'UIEventType'), ('listener', _LISTENER), ('options?', _LISTENER_OPTIONS)], '')]}
OVERRIDES[('UIElement', 'dispatchEvent')] = {
    'doc': ['Sends a `type` event from the element (custom names too); listeners read `detail` as',
            '`event.detail` (a copy). Returns false when a listener called preventDefault().'],
    'overloads': [([('type', 'string'), ('detail?', 'any'), ('bubbles?', 'boolean')], 'boolean')]}
OVERRIDES[('UIDocument', 'createDataModel')] = {
    'doc': ['Creates the data model `name` (data-model="name" in the document) from a table: functions',
            'become data-event callbacks (`data-event-click="buy(i)"` calls `buy(event, i)`), every other',
            'value the model\'s data, read and written through the returned proxy (`model.gold = 20`).',
            'Its top-level keys are fixed. A loaded document that uses the model reloads.'],
    'overloads': [([('name', 'string'), ('initial', 'table')], 'UIDataModel')]}
OVERRIDES[('UIDocument', 'getDataModel')] = {
    'doc': ['The proxy of a data model this document created, or nil.'],
    'overloads': [([('name', 'string')], 'UIDataModel|nil')]}
# anim-retarget: addClip's options table, Model:getHumanoidInfo's table (LuaRef in C++)
OVERRIDES[('AnimationPlayer', 'addClip')] = {
    'doc': ["Adds a clip entry (an authored change: the next update reloads): an asset-relative .anim or",
            "\"<model>#<clip>\"; `name` empty or nil = the default state name. `options.retarget` plays a",
            "generic clip as a humanoid clip, retargeted from its source model; `mirror` / `footIK` apply",
            "to humanoid clips; `layer` is the entry's layer. False when the state name is taken."],
    'overloads': [([('path', 'string')], 'boolean'),
                  ([('path', 'string'), ('name', 'string')], 'boolean'),
                  ([('path', 'string'), ('name', 'string|nil'),
                    ('options', '{retarget?: boolean, mirror?: boolean, footIK?: boolean, layer?: integer}')], 'boolean')]}
OVERRIDES[('RenderSystem', 'getLoadCount')] = {
    'doc': ['How many of the loads isAllLoaded() waits for are done, and how many there are (a loading screen\'s progress).'],
    'overloads': [([], 'integer, integer')]}
OVERRIDES[('Rigidbody', 'getAllowedDOFs')] = {
    'doc': ['The degrees of freedom setAllowedDOFs set, in its order: translationX, translationY, translationZ,',
            'rotationX, rotationY, rotationZ (all true by default).'],
    'overloads': [([], 'boolean, boolean, boolean, boolean, boolean, boolean')]}
OVERRIDES[('Model', 'getHumanoidInfo')] = {
    'doc': ["The model's humanoid rig (its Avatar): `humanoid` (the bone map is a valid humanoid),",
            "`animationType` (\"generic\" or \"humanoid\", the .humanoid sidecar's), `humanScale`, the unmapped",
            "slot names in `missing`, `extraMapped` (fingers, eyes and jaw, of 33) and `error`; nil without bones."],
    'overloads': [([], '{humanoid: boolean, animationType: string, humanScale: number, missing: string[], extraMapped: integer, error?: string}|nil')]}
OVERRIDES[('UIClassList', 'add')] = {
    'doc': ['Adds the classes.'], 'overloads': [([('...', 'string')], '')]}
OVERRIDES[('UIClassList', 'remove')] = {
    'doc': ['Removes the classes.'], 'overloads': [([('...', 'string')], '')]}

# HTML UI (phase 4, p4-guide): precise types of what the bindings return as `any` or plain strings
OVERRIDES[('UIDocument', 'getDiagnostics')] = {
    'doc': ['Every finding of the last (re)load, notes included, in source order (the Console shows errors and',
            'warnings). Codes: jasper://docs/html-ui-support.'],
    'overloads': [([], 'UIDiagnostic[]')]}
OVERRIDES[('UIDocument', 'getTargetTexture')] = {
    'doc': ['The texture a TEXTURE-mode document renders into (RGBA8, straight alpha), the same object while the',
            'component exists: give it to a Mesh, Primitive or Image with setTexture. nil for a SCREEN document.'],
    'overloads': [([], 'Framebuffer|nil')]}
OVERRIDES[('UIDocument', 'setColorScheme')] = {
    'doc': ['Switches prefers-color-scheme for every document of the scene ("light" by default).'],
    'overloads': [([('scheme', '"light"|"dark"')], '')]}
OVERRIDES[('UIElement', 'insertAdjacentHTML')] = {
    'doc': ['Parses `html` (like innerHTML) and inserts it relative to the element.'],
    'overloads': [([('position', '"beforebegin"|"afterbegin"|"beforeend"|"afterend"'), ('html', 'string')], '')]}

# HTML UI effects (phase 5, p5-infra, spec 3.12): the effect clock and an element's effects
OVERRIDES[('UIElement', 'getEffects')] = {
    'doc': ['The effects the element uses (effect() in background-image, mask-image, filter, backdrop-filter),',
            'in property order, with the canonical CSS text of every parameter (universal ones included).',
            'Catalogue: jasper://docs/html-ui-effects.'],
    'overloads': [([], 'UIEffectInfo[]')]}
# phase 6 (docs/html-ui/phase-6-stateful-effects.md 3.8): inputs and resets of stateful effects
OVERRIDES[('UIElement', 'effectSplat')] = {
    'doc': ['Injects a splat into the element\'s stateful effects (fluid-smoke, ink-flow, boids, ...): a push',
            'of dye and velocity they take at their next step (logged and replayed under a fixed effect time).',
            'x, y, radius in the effect area\'s CSS px; dx, dy px/s; effect names one effect (default: all).',
            'Returns how many simulations took it.'],
    'overloads': [([('splat', 'UIEffectSplat')], 'integer')]}
OVERRIDES[('UIElement', 'effectReset')] = {
    'doc': ['Restarts the element\'s stateful effects (or the one named) from their initial state; returns how many.'],
    'overloads': [([('effect', 'string?')], 'integer')]}
OVERRIDES[('UIDocument', 'setEffectTime')] = {
    'doc': ['Freezes every HTML UI effect at `seconds` of effect time (screenshots, tests); nil runs them again.',
            'Only the effects: CSS animations, transitions and anime keep running.'],
    'overloads': [([('seconds', 'number?')], '')]}
OVERRIDES[('UIDocument', 'getEffectTime')] = {
    'doc': ['The effects\' clock in seconds: the fixed time when one is set, else the UI clock.'],
    'overloads': [([], 'number')]}

# HTML UI widgets (S17): the lua_CFunctions of the views, tooltips, drag and drop and reloads
OVERRIDES[('UIItemsView', 'setProvider')] = {
    'doc': ['The rows from Lua instead of a data model: `count` items, `bind(row, i)` fills a row element for',
            'item i (1-based), `unbind(row, i)` clears it, `key(i)` names an item (selection, row identity),',
            '`height(i)` gives a known height. A <tree-view> asks `children(key)` (nil: the roots) for the keys',
            'of a node\'s children and `hasChildren(key)`; its bind gets the flat index (getNode(i)).'],
    'overloads': [([('provider', 'UIItemsProvider')], '')]}
OVERRIDES[('UIItemsView', 'getVisibleRange')] = {
    'doc': ['The first and last visible items (1-based; 0, 0 when the view is empty).'],
    'overloads': [([], 'integer, integer')]}
OVERRIDES[('UIItemsView', 'getSelection')] = {
    'doc': ['The selected items (1-based, ascending).'], 'overloads': [([], 'integer[]')]}
OVERRIDES[('UIItemsView', 'getSelectedKeys')] = {
    'doc': ['The keys of the selected items.'], 'overloads': [([], 'string[]')]}
OVERRIDES[('UIItemsView', 'getNode')] = {
    'doc': ['A <tree-view> row\'s node: its key, depth (0 for a root), expanded, hasChildren and parent (flat index, 0 for a root).'],
    'overloads': [([('index', 'integer')], '{key: string, depth: integer, expanded: boolean, hasChildren: boolean, parent: integer}|nil')]}
OVERRIDES[('UIItemsView', 'getColumns')] = {
    'doc': ['A <table-view>\'s columns (<table-column>): key, width, minWidth, sortable, resizable.'],
    'overloads': [([], '{key: string, width: number, minWidth: number, sortable: boolean, resizable: boolean}[]')]}
OVERRIDES[('UIItemsView', 'getState')] = {
    'doc': ['What inspect_ui reports of the view: kind, count, first, last, pool, poolBound, overscan, lanes,',
            'selection, selected, selectedKeys, active, scrollTop, totalHeight, source, layoutBoundary, rowAliases.'],
    'overloads': [([], 'table')]}
for _key_method in ('indexOfKey', 'expand', 'collapse', 'toggle', 'isExpanded'):
    OVERRIDES[('UIItemsView', _key_method)] = {
        'doc': {'indexOfKey': ['The item with this key (1-based), 0 for none.'],
                'expand': ['Expands a <tree-view> node (the expand event fires); false when it has no children or is open.'],
                'collapse': ['Collapses a <tree-view> node (the collapse event fires); false when it is closed.'],
                'toggle': ['Expands or collapses a <tree-view> node.'],
                'isExpanded': ['A <tree-view> node is expanded.']}[_key_method],
        'overloads': [([('key', 'string|integer')], 'integer' if _key_method == 'indexOfKey' else 'boolean')]}
OVERRIDES[('UIElement', 'setTooltip')] = {
    'doc': ['A rich tooltip in place of the title attribute: a text, a copy of an element (made on show), or a',
            'function called on each show that returns a text or an element (nil: none); nil removes it.',
            '`options`: placement ("top" default, "bottom", "left", "right", "auto"), delay (s), arrow, interactive.'],
    'overloads': [([('content', 'string|UIElement|fun(): string|UIElement|nil|nil'), ('options?', 'UITooltipOptions')], '')]}
OVERRIDES[('UIDataTransfer', 'setData')] = {
    'doc': ['Puts a value of a type ("item", "text/plain") in the drag: any Lua value, kept by reference until',
            'the drag ends (a table arrives by identity).'],
    'overloads': [([('type', 'string'), ('value', 'any')], '')]}
OVERRIDES[('UIDataTransfer', 'getData')] = {
    'doc': ['The value of a type, or nil (also in dragenter and dragover, unlike HTML5). "htmlui/row" is',
            '`{view = <the view>, index = <1-based item>, key = <its key>}` for a drag from a virtual view\'s row.'],
    'overloads': [([('type', 'string')], 'any')]}
OVERRIDES[('UIDataTransfer', 'setDragImage')] = {
    'doc': ['The ghost: a copy of `element` with the pointer at (x, y) in it; nil draws none.'],
    'overloads': [([('element', 'UIElement|nil'), ('x?', 'number'), ('y?', 'number')], '')]}
PROPERTY_OVERRIDES[('UIDataTransfer', 'types')] = ('string[]', True)
OVERRIDES[('UIDrag', 'begin')] = {
    'doc': ['Starts a drag from `source` without a pointer (gamepad, scripts): dragstart and drag fire on it.',
            '`options.data` is a table of type -> value, `options.effectAllowed` a DataTransfer effect. False when',
            'a dragstart listener prevented it.'],
    'overloads': [([('source', 'UIElement'), ('options?', '{data: table<string, any>|nil, effectAllowed: string|nil}')], 'boolean')]}
OVERRIDES[('UIDrag', 'over')] = {
    'doc': ['Moves the drag over `target` (dragleave/dragenter as it changes, then dragover); true when it accepts.'],
    'overloads': [([('target', 'UIElement')], 'boolean')]}
OVERRIDES[('UIDrag', 'drop')] = {
    'doc': ['Drops on `target` (drop when it accepts, else dragleave), then dragend; true when it was dropped.'],
    'overloads': [([('target', 'UIElement')], 'boolean')]}
OVERRIDES[('UIDocumentTooltip', 'configure')] = {
    'doc': ['The document\'s tooltips: `enabled` (title tooltips too), `delay` and `hopWindow` in seconds (0.5).'],
    'overloads': [([('options', '{enabled: boolean|nil, delay: number|nil, hopWindow: number|nil}')], '')]}
OVERRIDES[('UIDocument', 'reload')] = {
    'doc': ['Loads the document again, keeping scroll, focus, form values and listeners. An .html document is',
            'patched in place (`mode = "patch"`, the default: elements, handles and listeners stay, onReload fires)',
            'or loaded again (`mode = "rebuild"`: onUnload and onLoad fire).'],
    'overloads': [([], ''), ([('options', '{mode: "patch"|"rebuild"}')], '')]}
PROPERTY_OVERRIDES[('UIDocument', 'onReload')] = ('fun(report: UIReloadReport)|nil', False)

# S76 runtime areas (NavigationComponentsLua.cpp): script volumes
OVERRIDES[('NavigationSystem', 'setAreaInVolume')] = {
    'doc': ['A script volume (a world box): the navmesh polygons whose centre lies in it take `area` (1 Not',
            'Walkable: nothing walks there) until `remove()`, on every surface of `options.agentType` (all by',
            'default); it ends at Stop. nil for an area outside 0-15.'],
    'overloads': [([('center', 'Vector3'), ('size', 'Vector3'), ('area', 'integer'),
                    ('options?', '{rotation: Quaternion|nil, agentType: string|integer|nil}')], 'NavAreaOverride|nil')]}

# S19 navigation: the queries returning (bool, hit) and the link data table
_NAV_FILTER = 'NavMeshQueryFilter|integer'
OVERRIDES[('NavigationSystem', 'samplePosition')] = {
    'doc': ['The nearest navmesh point within maxDistance of `point` (Unity\'s NavMesh.SamplePosition):',
            'found, then the hit (position, normal, distance, area, mask, hit). The filter: a',
            'NavMeshQueryFilter or an area mask (all areas of the first agent type by default).'],
    'overloads': [([('point', 'Vector3'), ('maxDistance', 'number'), ('filter?', _NAV_FILTER)], 'boolean, NavMeshHit')]}
OVERRIDES[('NavigationSystem', 'raycast')] = {
    'doc': ['Walks the navmesh surface from `from` to `to` (Unity\'s NavMesh.Raycast): blocked when an',
            'edge stops it before `to`, then the hit (where it stopped, the edge normal).'],
    'overloads': [([('from', 'Vector3'), ('to', 'Vector3'), ('filter?', _NAV_FILTER)], 'boolean, NavMeshHit')]}
OVERRIDES[('NavigationSystem', 'findClosestEdge')] = {
    'doc': ['The navmesh boundary point closest to `point` (Unity\'s NavMesh.FindClosestEdge): found, then',
            'the hit (position, normal, distance).'],
    'overloads': [([('point', 'Vector3'), ('filter?', _NAV_FILTER)], 'boolean, NavMeshHit')]}
PROPERTY_OVERRIDES[('NavMeshAgent', 'currentOffMeshLinkData')] = ('OffMeshLinkData', False)
# S75 navigation diagnostics: path reasons, isReachable's reason, locate, coverage, islands, the bake report
OVERRIDES[('NavigationSystem', 'isReachable')] = {
    'doc': ['Whether a Complete path joins the points, then why not ("none" when it does): rejected at once',
            'when their islands differ.'],
    'overloads': [([('from', 'Vector3'), ('to', 'Vector3'), ('filter?', _NAV_FILTER)], 'boolean, string')]}
OVERRIDES[('NavigationSystem', 'locate')] = {
    'doc': ['Where `point` is on the navmesh: onMesh when a path from here would start, else the nearest',
            'point within maxDistance (5) with its snap distance, island, area and surface.'],
    'overloads': [([('point', 'Vector3'), ('filter?', _NAV_FILTER), ('maxDistance?', 'number')], 'NavLocation')]}
OVERRIDES[('NavigationSystem', 'getCoverage')] = {
    'doc': ['The walkable area of a surface (m\u00b2), its island count, the largest island and the share of its',
            'tiles\' footprint that is walkable.'],
    'overloads': [([('surface', 'integer')], 'NavCoverage')]}
OVERRIDES[('NavigationSystem', 'getIslands')] = {
    'doc': ['The connected parts of a surface\'s navmesh (links included): id, area (m\u00b2), polygons, world bounds.'],
    'overloads': [([('surface', 'integer')], 'NavIsland[]')]}
PROPERTY_OVERRIDES[('NavMeshSurface', 'lastBakeResult')] = ('NavBakeReport|nil', True)

# S76 navigation queries: random points, surface moves, the closest edge within a distance, path
# options (docs/specs/S76-navigation-gameplay-api.md R1-R5, R42)
OVERRIDES[('NavigationSystem', 'calculatePath')] = {
    'doc': ['A path from `from` to `to` (Unity\'s NavMesh.CalculatePath): status, corners, length, cost, cornerAreas,',
            'cornerFlags, areaLengths and the corridor NavMeshAgent:setPath takes; when it is not Complete why',
            '(`reason`, "disconnected", ...), the answering surface, the closest point and the gap left to the end.',
            '`options.areaCrossings` adds a corner wherever the path changes area.'],
    'overloads': [([('from', 'Vector3'), ('to', 'Vector3'), ('filter?', _NAV_FILTER), ('options?', 'NavMeshPathOptions')], 'NavMeshPath')]}
OVERRIDES[('NavigationSystem', 'findClosestEdge')] = {
    'doc': ['The navmesh boundary point closest to `point` (Unity\'s NavMesh.FindClosestEdge): found, then',
            'the hit (position, normal from the wall towards the point, distance). With maxDistance only walls',
            'that close count: none gives false with hit.distance = maxDistance.'],
    'overloads': [([('point', 'Vector3'), ('filter?', _NAV_FILTER), ('maxDistance?', 'number')], 'boolean, NavMeshHit')]}
OVERRIDES[('NavigationSystem', 'randomPoint')] = {
    'doc': ['A random navmesh point: anywhere (uniform by area), around `options.center` within `options.radius`',
            '(reachable from it, not uniform) and/or on the island of `options.sameIslandAs`. Draws come from the',
            'system\'s stream (randomSeed), or from `options.seed`\'s own. Found, then the hit.'],
    'overloads': [([('filter?', _NAV_FILTER + '|nil'), ('options?', 'NavRandomPointOptions')], 'boolean, NavMeshHit')]}
OVERRIDES[('NavigationSystem', 'moveAlongSurface')] = {
    'doc': ['A controller step along the navmesh from `from` towards `to` (at most 64 polygons): moved, then the',
            'hit (position on the surface, hit = stopped short of `to` by a wall, distance moved, area). False when',
            '`from` is off the mesh.'],
    'overloads': [([('from', 'Vector3'), ('to', 'Vector3'), ('filter?', _NAV_FILTER)], 'boolean, NavMeshHit')]}
PROPERTY_OVERRIDES[('UIDocument', 'lastReloadReport')] = ('UIReloadReport|nil', False)

# S76 agents: the NavMeshAgent's paths, queries and link data (docs/specs/S76-navigation-gameplay-api.md
# R19-R27)
OVERRIDES[('NavMeshAgent', 'calculatePath')] = {
    'doc': ['A path from where the agent stands to `target` (snapped as setDestination snaps it), planned now',
            'on its surface with its area mask and costs (Unity\'s CalculatePath). Its own path is unchanged.'],
    'overloads': [([('target', 'Vector3')], 'NavMeshPath')]}
OVERRIDES[('NavMeshAgent', 'setPath')] = {
    'doc': ['Follows a path of calculatePath (Unity\'s SetPath): planned on its surface, still open to it, and',
            'starting where it stands. False when refused (a script-built table has no corridor); the agent',
            'then has no path.'],
    'overloads': [([('path', 'NavMeshPath')], 'boolean')]}
OVERRIDES[('NavMeshAgent', 'raycast')] = {
    'doc': ['NavigationSystem:raycast from where the agent stands with its filter: blocked, then the hit.'],
    'overloads': [([('target', 'Vector3')], 'boolean, NavMeshHit')]}
OVERRIDES[('NavMeshAgent', 'findClosestEdge')] = {
    'doc': ['The wall nearest the agent with its filter: found, then the hit.'],
    'overloads': [([], 'boolean, NavMeshHit')]}
OVERRIDES[('NavMeshAgent', 'samplePathPosition')] = {
    'doc': ['Walks its path up to maxDistance: true, with the point where the path first enters an area',
            'outside areaMask, else false with the point maxDistance along it (or its end).'],
    'overloads': [([('areaMask', 'integer'), ('maxDistance', 'number')], 'boolean, NavMeshHit')]}
PROPERTY_OVERRIDES[('NavMeshAgent', 'path')] = ('NavMeshPath', False)
PROPERTY_OVERRIDES[('NavMeshAgent', 'nextOffMeshLinkData')] = ('OffMeshLinkData', False)

# HTML UI (phase 4, p4-guide): class doc lines appended to the generated ones (the guide's URI)
HTML_UI_CLASS_DOCS = {
    'UIDocument': ['Get it with `UIDocument(self.scene, self.entity)` in a script on the document\'s GameObject.',
                   'Guide: jasper://docs/html-ui (Lua API, data binding, events); CSS support: jasper://docs/html-ui-support.'],
    'UIElement': ['Guide: jasper://docs/html-ui ("Lua API"). Members use colons: el:querySelector(".x"), el.classList:add("on").'],
    'UIEvent': ['Guide: jasper://docs/html-ui ("Lua API", events table).'],
    'anime': ['Anime.js v4 for HTML UI elements, on the UI clock (unscaled game time). Guide: jasper://docs/html-ui',
              '("Animation"). Call with a dot: anime.animate("#title", {opacity = {from = 0}, duration = 400}).'],
}

# HTML UI (phase 4, p4-guide): field docs and types replacing what the C++ comments say
HTML_UI_FIELD_OVERRIDES = {
    ('UIElement', 'tagName'): {'doc': ['the DOM name: upper case in .html documents ("DIV", "BUTTON"), lower case in .rml']},
    ('UIElement', 'innerHTML'): {'doc': ['the children as HTML (parsed and rewritten like the document; RML in .rml documents)']},
    ('UIElement', 'textContent'): {'doc': ['the text of the descendants; assigning replaces the children with that text (never parsed)']},
    ('UIDocument', 'body'): {'doc': ['the <body> element (the root of an .rml document)']},
    ('UIDocument', 'documentElement'): {'doc': ['the <html> element (CSS :root; the same as body in an .rml document)']},
    ('UIEvent', 'gamepad'): {'doc': ['the id of the gamepad that sent this keydown/keyup (code "GamepadA", ...), -1 for the keyboard']},
}

# HTML UI (phase 4, p4-guide): the event names of UIElement/UIDocument addEventListener
# (engine/core/object/ui/HtmlUIEventListener.cpp, RmlUi's EventSpecification and the engine's
# own elements, with the engine-gaps fork's Pointer Events and mouseenter / mouseleave). touch*,
# contextmenu, beforeinput and
# composition* never reach documents (addEventListener warns), so they are not listed.
HTML_UI_EVENT_TYPES = [
    ('click', 'a press and release on the element (Enter/Space on a focused button too)'),
    ('dblclick', 'a double click'),
    ('mousedown', 'a pointer press (event.button: 0 left, 1 middle, 2 right; every finger too, event.pointerId)'),
    ('mouseup', 'a pointer release'),
    ('mousemove', 'the pointer moved over the element'),
    ('mouseover', 'the pointer entered the element or a child (bubbles)'),
    ('mouseout', 'the pointer left the element or a child (bubbles)'),
    ('wheel', 'the mouse wheel (event.deltaX/deltaY in steps, positive down/right)'),
    ('pointerdown', 'a pointer pressed: the mouse (pointerId 1) or a finger (event.pointerType "touch", event.fingerId); each finger its own'),
    ('pointermove', 'the pointer moved (to its capture when captured)'),
    ('pointerup', 'the pointer released'),
    ('pointercancel', 'the pointer ended without a release (a touch that started a pan, a cancelled touch)'),
    ('pointerover', 'the pointer came over the element (bubbles)'),
    ('pointerout', 'the pointer left the element (bubbles)'),
    ('pointerenter', 'the pointer entered the element or a child (does not bubble)'),
    ('pointerleave', 'the pointer left the element and its children (does not bubble)'),
    ('gotpointercapture', 'the element captured the pointer (setPointerCapture, or a touch\'s pointerdown target)'),
    ('lostpointercapture', 'the element lost the pointer\'s capture (release, up, cancel)'),
    ('mouseenter', 'the pointer entered the element (does not bubble)'),
    ('mouseleave', 'the pointer left the element (does not bubble)'),
    ('keydown', 'a key press on the focused element (event.key: "Enter", "Escape", "ArrowUp", "a"); gamepads too (event.code: "GamepadA", "GamepadDPadUp", ...)'),
    ('keyup', 'a key release (keyboard or gamepad)'),
    ('textinput', 'text typed into the focused element (event.data)'),
    ('focus', 'the element got the focus'),
    ('blur', 'the element lost the focus'),
    ('input', 'every edit of a text field, every toggle of a checkbox or radio'),
    ('change', 'a committed value: text fields on blur, checkboxes, radios, selects and ranges at once'),
    ('submit', 'a form was submitted (button or Enter in a text field)'),
    ('reset', 'a form was reset'),
    ('scroll', 'the element scrolled'),
    ('resize', 'the element changed size'),
    ('load', 'the document (or a <lottie> animation) loaded'),
    ('unload', 'the document unloads'),
    ('show', 'the document was shown'),
    ('hide', 'the document was hidden'),
    ('dragstart', 'a drag began on the element (draggable="true"; event.dataTransfer:setData); preventDefault cancels it'),
    ('drag', 'the dragged element moved'),
    ('dragend', 'the drag ended (event.dataTransfer.dropEffect: "none" when cancelled or not dropped)'),
    ('dragenter', 'a drag entered this element'),
    ('dragover', 'a drag is over this element (on entry and as it moves, once a frame): preventDefault accepts it'),
    ('dragleave', 'a drag left this element'),
    ('drop', 'a drag this element accepted was dropped on it (event.dataTransfer:getData)'),
    ('dragmove', 'RmlUi: a drag="drag-drop" element moved over this element'),
    ('dragout', 'RmlUi: a dragged element left this element'),
    ('dragdrop', 'RmlUi: a dragged element was dropped on this element'),
    ('tooltipshow', 'the element\'s tooltip showed'),
    ('tooltiphide', 'the element\'s tooltip hid'),
    ('select', 'a virtual view\'s selection changed (event.detail: {indices, keys})'),
    ('activate', 'a virtual view\'s item was activated: Enter, gamepad A, double click (event.detail: {index, key})'),
    ('expand', 'a <tree-view> node expanded (event.detail: {index, key})'),
    ('collapse', 'a <tree-view> node collapsed (event.detail: {index, key})'),
    ('sort', 'a <table-view> header asked to sort (event.detail: {key, direction}): sort the data'),
    ('columnresize', 'a <table-view> column was resized (event.detail: {key, width})'),
    ('animationstart', 'a CSS animation started (event.animationName)'),
    ('animationiteration', 'a CSS animation began a new iteration'),
    ('animationend', 'a CSS animation ended'),
    ('animationcancel', 'a CSS animation was removed before its end'),
    ('transitionrun', 'a CSS transition was created (event.propertyName)'),
    ('transitionstart', 'a CSS transition began (after its delay)'),
    ('transitionend', 'a CSS transition ended'),
    ('transitioncancel', 'a CSS transition was removed before its end'),
    ('toggle', 'a <details> opened or closed'),
    ('close', 'a <dialog> closed'),
    ('cancel', 'Escape closes a modal <dialog> (close follows)'),
    ('htmlnavigate', 'a link was activated (preventDefault keeps the document)'),
    ('play', '<lottie>: playback started'),
    ('pause', '<lottie>: playback stopped'),
    ('loop', '<lottie>: an iteration ended and the next began'),
    ('complete', '<lottie>: the last iteration ended'),
    ('error', '<lottie>: the animation could not load (event.detail.message)'),
]


# Precise LuaLS types of parameters whose C++ type says nothing (luabridge::LuaRef becomes
# `any`): (Lua class, member, parameter name) -> Lua type. A `luabridge::LuaRef` parameter of a
# bound function that takes a callback or a parameter table belongs here.
_PARAM_TYPE_OVERRIDES = {
    # GameObject:setParent / parent =: a GameObject (any handle), a script or nil (the root)
    ('GameObject', 'setParent', 'parent'): 'EntityHandle|ScriptInstance|nil',
    # 2D physics materials: a .physicsmaterial path, a PhysicsMaterial or nil (none)
    ('Rigidbody2D', 'setColliderMaterial', 'value'): 'string|PhysicsMaterial|nil',
    ('Rigidbody', 'setColliderMaterial', 'value'): 'string|PhysicsMaterial|nil',
    ('PhysicsMaterial', 'new', 'params'): '{friction?: number, bounciness?: number, frictionCombine?: PhysicsMaterialCombine, bounceCombine?: PhysicsMaterialCombine}|nil',
}
# anime (engine/core/script/binding/HtmlUIAnimeLua.cpp, phase-1 p1-anim): the parameter tables
# are the Anime*Params classes of engine/core/script/binding/stubs/anime.lua
_ANIME_TARGETS = 'AnimeTargets'
OVERRIDES.update({
    ('anime', 'animate'): {
        'doc': ['Animates `targets` (a CSS selector, a UIElement, a plain table, or a list of them) as',
                '`params` says: properties to animate plus duration, delay, ease, loop, callbacks ... (Anime.js v4).'],
        'overloads': [([('targets', _ANIME_TARGETS), ('params', 'AnimeAnimationParams')], 'AnimeAnimation')]},
    ('anime', 'createTimeline'): {
        'doc': ['A timeline: animations, timers, calls and labels placed at positions.'],
        'overloads': [([('params?', 'AnimeTimelineParams')], 'AnimeTimeline')]},
    ('anime', 'createTimer'): {
        'doc': ['A timer: a duration with loops and callbacks, animating nothing.'],
        'overloads': [([('params?', 'AnimeTimerParams')], 'AnimeTimer')]},
    ('anime', 'createSpring'): {
        'doc': ['A spring easing (`ease = spring`); the animation lasts its settling duration.'],
        'overloads': [([('params?', 'AnimeSpringParams')], 'AnimeSpring')]},
    ('anime', 'spring'): {
        'doc': ['Same as anime.createSpring.'],
        'overloads': [([('params?', 'AnimeSpringParams')], 'AnimeSpring')]},
    ('anime', 'stagger'): {
        'doc': ['A function value spreading `value` (or the range {from, to}) over the targets: for',
                'delays, property values or timeline positions.'],
        'overloads': [([('value', 'number|string|number[]|string[]'), ('params?', 'AnimeStaggerParams')], 'AnimeStagger')]},
    ('anime', 'cubicBezier'): {
        'doc': ['A cubic Bézier easing (CSS cubic-bezier()).'],
        'overloads': [([('x1', 'number'), ('y1', 'number'), ('x2', 'number'), ('y2', 'number')], 'AnimeEase')]},
    ('anime', 'steps'): {
        'doc': ['A stepped easing (CSS steps(); jump-start when `fromStart`).'],
        'overloads': [([('steps', 'integer'), ('fromStart?', 'boolean')], 'AnimeEase')]},
    ('anime', 'linear'): {
        'doc': ['A piecewise linear easing through the points (numbers, or "value percent%" strings).'],
        'overloads': [([('...', 'number|string')], 'AnimeEase')]},
    ('anime', 'irregular'): {
        'doc': ['A random steps easing.'],
        'overloads': [([('length?', 'integer'), ('randomness?', 'number')], 'AnimeEase')]},
    ('AnimeEase', '__call'): {
        'doc': ['ease(t): the eased value; for the parametric eases (in, out, inOut, outIn, *Back, *Elastic)',
                'ease(params...) makes a new ease (anime.eases.outElastic(1, .6)).'],
        'overloads': [([('t', 'number')], 'number'), ([('...', 'number')], 'AnimeEase')]},
    ('AnimeStagger', '__call'): {
        'doc': ['The staggered value of the target at `index` (0-based, as Anime) of `total`.'],
        'overloads': [([('target', 'any'), ('index', 'integer'), ('total?', 'integer')], 'number|string')]},
    ('AnimeTimer', 'andThen'): {
        'doc': ['Calls `fn(self)` once completed (at once when already completed): Anime\'s then(),',
                'renamed because `then` is a Lua keyword.'],
        'overloads': [([('fn', 'fun(timer: AnimeTimer)')], 'AnimeTimer')]},
    ('AnimeTimeline', 'add'): {
        'doc': ['Adds an animation of `targets` at `position` (default: the end), or a timer',
                '(`timerParams`), or syncs an anime object. A stagger position adds one child per target.'],
        'overloads': [([('targets', _ANIME_TARGETS), ('params', 'AnimeAnimationParams'), ('position?', 'AnimeTimelinePosition')], 'AnimeTimeline'),
                      ([('timerParams', 'AnimeTimerParams'), ('position?', 'AnimeTimelinePosition')], 'AnimeTimeline'),
                      ([('timer', 'AnimeTimer'), ('position?', 'AnimeTimelinePosition')], 'AnimeTimeline')]},
    ('AnimeTimeline', 'set'): {
        'doc': ['Sets `props` on `targets` at `position`.'],
        'overloads': [([('targets', _ANIME_TARGETS), ('props', 'AnimeAnimationParams'), ('position?', 'AnimeTimelinePosition')], 'AnimeTimeline')]},
    ('AnimeTimeline', 'call'): {
        'doc': ['Calls `fn(timeline)` when the playhead crosses `position`.'],
        'overloads': [([('fn', 'fun(timeline: AnimeTimeline)'), ('position?', 'AnimeTimelinePosition')], 'AnimeTimeline')]},
    ('AnimeTimeline', 'sync'): {
        'doc': ['Drives `timer` (an animation, timer or timeline) from this timeline, from `position`.'],
        'overloads': [([('timer', 'AnimeTimer'), ('position?', 'AnimeTimelinePosition')], 'AnimeTimeline')]},
    ('AnimeTimeline', 'remove'): {
        'doc': ['Removes the tweens of `targets` (of `property` only, when given) from the timeline.'],
        'overloads': [([('targets', _ANIME_TARGETS), ('property?', 'string')], 'AnimeTimeline')]},
})

# Handler arguments of Engine events, by name (types come from the C++ declaration)
ENGINE_EVENT_ARGS = {
    'onKeyDown': ['key', 'isRepeat', 'mods'], 'onKeyUp': ['key', 'isRepeat', 'mods'],
    'onCharInput': ['char'],
    'onMouseDown': ['button', 'x', 'y', 'mods'], 'onMouseUp': ['button', 'x', 'y', 'mods'],
    'onMouseMove': ['x', 'y', 'mods'], 'onMouseScroll': ['xoffset', 'yoffset', 'mods'],
    'onTouchStart': ['pointer', 'x', 'y'], 'onTouchMove': ['pointer', 'x', 'y'],
    'onTouchEnd': ['pointer', 'x', 'y'], 'onTouchCancel': ['pointer', 'x', 'y'],
    'onGamepadConnect': ['gamepad'], 'onGamepadDisconnect': ['gamepad'],
    'onGamepadButtonDown': ['gamepad', 'button'], 'onGamepadButtonUp': ['gamepad', 'button'],
    'onGamepadAxisMove': ['gamepad', 'axis', 'value'],
    'onButtonDown': ['action'], 'onButtonUp': ['action'],
}

# Handler arguments of other classes' event fields, by (class, field)
EVENT_FIELD_ARGS = {
    ('Log', 'onMessageReceived'): ['message', 'stackTrace', 'level'],
    ('Audio', 'onMusicBeat'): ['bar', 'beat'],
    ('Audio', 'onMusicSection'): ['section', 'clip'],
    ('Device', 'onThermalStateChanged'): ['state'],
}

_LUA_KEYWORDS = {'and', 'break', 'do', 'else', 'elseif', 'end', 'false', 'for', 'function', 'goto', 'if',
                 'in', 'local', 'nil', 'not', 'or', 'repeat', 'return', 'then', 'true', 'until', 'while'}

_OPERATORS = {'__add': 'add', '__sub': 'sub', '__mul': 'mul', '__div': 'div', '__mod': 'mod', '__pow': 'pow',
              '__unm': 'unm', '__idiv': 'idiv', '__band': 'band', '__bor': 'bor', '__bxor': 'bxor',
              '__shl': 'shl', '__shr': 'shr', '__bnot': 'bnot', '__concat': 'concat', '__len': 'len'}

_PHYSICS_NOTES = {
    'defined(JASPERENGINE_PHYSICS_2D)': 'Only in builds with 2D physics (Project Settings > Build > 2D Physics).',
    'defined(JASPERENGINE_PHYSICS_3D)': 'Only in builds with 3D physics (Project Settings > Build > 3D Physics).',
}


# ═══════════════════════════════════════════════════════════════════════════════
# Lua model
# ═══════════════════════════════════════════════════════════════════════════════

class LuaSig:
    """A Lua-visible signature: params [(name, lua type, optional)], returns lua type or ''."""
    __slots__ = ('params', 'ret', 'doc', 'vararg')

    def __init__(self, params, ret, doc=None, vararg=None):
        self.params = params
        self.ret = ret
        self.doc = doc or []
        self.vararg = vararg


def lua_name(name, used):
    n = re.sub(r'[^A-Za-z0-9_]', '_', name or '') or 'arg'
    if n in _LUA_KEYWORDS or n == 'self':
        n += '_'
    if n[0].isdigit():
        n = '_' + n
    base = n
    k = 2
    while n in used:
        n = f'{base}{k}'
        k += 1
    used.add(n)
    return n


class Builder:
    def __init__(self, model, bindings):
        self.model = model
        self.b = bindings
        self.r = Resolver(model, bindings)
        self.tm = TypeMapper(model, self.r)
        self.unresolved = []

    # ── enums ────────────────────────────────────────────────────────────────

    def build_enums(self):
        for en in self.b.enums:
            vals = []
            cpp_enum = None
            for name, expr, call in en.values:
                qual, vname = self.r.split_qualified([x for x in expr if x.t not in ('(', ')')]) if expr else (None, None)
                v = None
                if qual:
                    ce = self.model.find_enum(qual)
                    if ce is not None:
                        cpp_enum = cpp_enum or ce
                        for cn, cv, _ in ce.values:
                            if cn == vname:
                                v = cv
                                break
                if v is None and expr and not qual:
                    # a literal value (a namespace of plain constants: ParticleSystemStopBehavior)
                    v = eval_int(expr, {}, self.model)
                vals.append((name, v, call))
            en.resolved = vals
            en.cpp_enum = cpp_enum
            if cpp_enum is not None:
                self.r.enum_ns[cpp_enum.name] = en.name

    # ── classes ──────────────────────────────────────────────────────────────

    def build(self):
        self.build_enums()
        by_cpp = {type_key(c.cpp): c for c in self.b.classes}
        for c in self.b.classes:
            if c.base_cpp:
                bc = by_cpp.get(type_key(c.base_cpp))
                c.base = bc.name if bc else None
        for c in self.b.classes:
            c.cpp_class = self.model.find_class(c.cpp)
            c.lua_ctors = []
            for call in c.ctors:
                for targ in call.targs:
                    c.lua_ctors.append(self.ctor_sig(c, targ))
                # placement lambdas (`[](void* memory, Scene* scene) { return new (memory) T(scene); }`):
                # the parameters after the memory pointer
                if not call.targs:
                    for arg in call.args:
                        targ = self.lambda_ctor_targ(arg)
                        if targ is not None:
                            c.lua_ctors.append(self.ctor_sig(c, targ))
            fs = re.match(r'^FunctionSubscribe<(.*?)\((.*)\)>$', c.cpp) or \
                re.match(r'^FunctionSubscribe<(.*?)\((.*)\)>$', c.base_cpp or '')
            for m in c.members:
                if fs and m.kind == 'Method' and m.name in ('call', '__call', 'add', 'remove', 'has'):
                    self.subscribe_member(c, m, fs.group(1), fs.group(2))
                    continue
                if m.kind in ('Method', 'StaticMethod'):
                    self.function_member(c, m)
                else:
                    self.property_member(c, m)

    @staticmethod
    def lambda_ctor_targ(arg):
        # `void (params but the first)` of a placement-new constructor lambda, None for other args
        if not arg or arg[0].t != '[':
            return None
        k = skip_balanced(arg, 0)
        if k >= len(arg) or arg[k].t != '(':
            return None
        e = skip_balanced(arg, k)
        inner = arg[k + 1:e - 1]
        depth = 0
        cut = None
        for j, tok in enumerate(inner):
            if tok.t in _OPEN:
                depth += 1
            elif tok.t in _CLOSE:
                depth -= 1
            elif tok.t == ',' and depth == 0:
                cut = j
                break
        first = inner if cut is None else inner[:cut]
        if not any(tok.t == 'void' for tok in first) or not any(tok.t == '*' for tok in first):
            return None
        rest = [] if cut is None else inner[cut + 1:]
        line, cond = arg[0].line, arg[0].cond
        return [Tok('id', 'void', line, cond), Tok('op', '(', line, cond)] + rest + [Tok('op', ')', line, cond)]

    def ctor_sig(self, c, targ):
        # `void(A, B)` or `void (*) (A, B)`: the last top-level parenthesized group
        groups = []
        k = 0
        while k < len(targ):
            if targ[k].t == '(':
                e = skip_balanced(targ, k)
                groups.append((k, e))
                k = e
                continue
            k += 1
        if not groups:
            return LuaSig([], c.name)
        s, e = groups[-1]
        params = parse_params(targ[s + 1:e - 1])
        names = [None] * len(params)
        doc = []
        if c.cpp_class is not None:
            for fn in c.cpp_class.methods.get(c.cpp_class.name, []):
                if fn.ctor and len(fn.params) == len(params) and \
                        all(type_key(a.type) == type_key(b.type) for a, b in zip(fn.params, params)):
                    names = [p.name for p in fn.params]
                    doc = fn.doc
                    break
        used = set()
        lp = []
        for p, nm in zip(params, names):
            if bare_type(p.type)[0] == 'lua_State':
                continue
            lp.append((lua_name(nm or self.default_param_name(p.type, len(lp)), used), self.tm.lua(p.type, c.cpp) or 'any', False))
        return LuaSig(lp, c.name, doc)

    def default_param_name(self, cpp, index):
        lt = self.tm.lua(cpp)
        if lt in self.r.lua_classes:
            return lt[0].lower() + lt[1:]
        return f'arg{index + 1}'

    def to_lua_sig(self, sig, c, drop_self, optional_bools, member=None):
        params = list(sig.params)
        if drop_self and params:
            params = params[1:]
        params = [p for p in params if bare_type(p[0])[0] != 'lua_State']
        used = set()
        lp = []
        for idx, (t, nm) in enumerate(params):
            lt = self.tm.lua(t, c.cpp) or 'any'
            desc = ''
            if lt == 'any' and bare_type(t)[0] not in ('LuaRef', 'luabridge::LuaRef'):
                desc = f'C++ {bare_type(t)[0]} (not a Lua class)'
            name = lua_name(nm or self.default_param_name(t, idx), used)
            override = _PARAM_TYPE_OVERRIDES.get((c.name, member.name if member else None, nm or name))
            if override:
                lt, desc = override, ''
            lp.append((name, lt, False, desc))
        # trailing booleans (nil reads false) and std::optional parameters (nil: the binding's
        # default) may be left out
        k = len(lp) - 1
        while k >= 0 and ((optional_bools and lp[k][1] == 'boolean') or _OPT.match(normalize_type(params[k][0]))):
            lp[k] = (lp[k][0], lp[k][1], True, lp[k][3])
            k -= 1
        ret = '' if sig.ret == '' else (self.tm.lua(sig.ret, c.cpp) if sig.ret is not None else 'any')
        return LuaSig(lp, ret, list(sig.doc))

    def function_member(self, c, m):
        key = (c.name, m.name)
        if key in OVERRIDES:
            ov = OVERRIDES[key]
            m.lua_sigs = [self.override_sig(o) for o in ov['overloads']]
            m.doc = m.call.doc + ov.get('doc', [])
            m.is_async = ov.get('async', False)
            return
        sigs = []
        m.cpp_sigs = []
        callables = m.call.args[1:]
        single = len(callables) == 1
        for arg in callables:
            res = self.r.resolve(arg, m)
            if res is None:
                self.unresolve(c, m, 'callable not resolved: ' + join_tokens(arg)[:80])
                continue
            kind, sig = res
            if sig.problem:
                self.unresolve(c, m, sig.problem)
                continue
            if sig.cfunc:
                self.unresolve(c, m, 'lua_CFunction needs a signature in OVERRIDES')
                continue
            if kind == 'data':
                self.unresolve(c, m, 'data member bound as a function')
                continue
            if kind == 'member' and any(bare_type(p[0])[0] == 'lua_State' for p in sig.params):
                self.unresolve(c, m, 'reads its arguments from the Lua stack: needs a signature in OVERRIDES')
                continue
            drop_self = kind == 'free' and m.kind == 'Method'
            sigs.append(self.to_lua_sig(sig, c, drop_self, single, m))
            # the C++ side, for the completion details (Type name, the C++ return type)
            defaults = sig.defaults if sig.defaults and len(sig.defaults) == len(sig.params) else [None] * len(sig.params)
            pairs = list(zip(sig.params, defaults))
            if drop_self and pairs:
                pairs = pairs[1:]
            pairs = [(q, d) for q, d in pairs if bare_type(q[0])[0] != 'lua_State']
            m.cpp_sigs.append(([q for q, _ in pairs], sig.ret, [d for _, d in pairs]))
        m.lua_sigs = sigs
        doc = list(m.call.doc)
        for s in sigs:
            for line in s.doc:
                if line not in doc:
                    doc.append(line)
        m.doc = doc

    def override_sig(self, o):
        params, ret = o
        lp = []
        vararg = None
        for nm, t in params:
            if nm == '...':
                vararg = t
                continue
            opt = nm.endswith('?')
            lp.append((nm.rstrip('?'), t, opt))
        return LuaSig(lp, ret, vararg=vararg)

    def subscribe_member(self, c, m, ret_cpp, args_cpp):
        args = [a for a in split_type_list(args_cpp) if a and a != 'void']
        ret = self.tm.lua(ret_cpp) if ret_cpp != 'void' else ''
        used = set()
        handler_params = [(lua_name(f'arg{i + 1}', used), self.handler_arg_type(a), False) for i, a in enumerate(args)]
        if m.name in ('call', '__call'):
            m.lua_sigs = [LuaSig(handler_params, ret)]
            m.doc = ['Calls every subscribed function with these arguments.'] if m.name == 'call' else []
        elif m.name == 'remove':
            m.lua_sigs = [LuaSig([('tag', 'string', False)], 'boolean')]
            m.doc = ['Removes the subscription with `tag` (UnregisterEvent computes it); true if there was one.']
            return
        elif m.name == 'has':
            m.lua_sigs = [LuaSig([('tag', 'string', False)], 'boolean')]
            m.doc = ['Whether a subscription with `tag` exists.']
            return
        else:
            fn = 'fun(' + ', '.join(f'{n}: {t}' for n, t, _ in handler_params) + ')' + (f': {ret}' if ret else '')
            m.lua_sigs = [LuaSig([('tag', 'string', False), ('fn', fn, False)], 'boolean')]
            m.doc = ['Subscribes `fn` under `tag` (an existing subscription with the same tag is replaced).']
        c.subscribe_sig = (handler_params, ret)

    def handler_arg_type(self, cpp):
        # LuaFunctionBase pushes a character (char32_t) as a UTF-8 string
        if bare_type(cpp)[0] in ('char32_t', 'wchar_t'):
            return 'string'
        return self.tm.lua(cpp) or 'any'

    def property_member(self, c, m):
        args = m.call.args
        getter = args[1] if len(args) > 1 else None
        setter = args[2] if len(args) > 2 else None
        static = m.kind == 'StaticProperty'
        m.nested_enum = None
        # a static property C++ reaches as Class::name: a static data member of that name, or a
        # constantCopy<T, T::NAME> of it (Engine.deltatime is Engine::getDeltatime() in C++)
        getter_text = join_tokens(getter) if getter else ''
        copied = re.search(r'constantCopy\s*<[^,]+,\s*[\w:]+::(\w+)\s*>', getter_text)
        m.cpp_visible = bool(copied and copied.group(1) == m.name)
        if getter and getter[0].t == '[' and any(x.t == 'addVariable' for x in getter):
            vals = []
            for k, x in enumerate(getter):
                if x.t == 'addVariable' and k + 1 < len(getter) and getter[k + 1].t == '(':
                    e = skip_balanced(getter, k + 1)
                    a = split_top(getter[k + 2:e - 1])
                    v = None
                    if len(a) == 2:
                        q, vn = self.r.split_qualified(a[1])
                        ce = self.model.find_enum(q) if q else None
                        if ce is not None:
                            v = next((cv for cn, cv, _ in ce.values if cn == vn), None)
                    vals.append((literal(a[0]), v))
            m.nested_enum = vals
            m.cpp_visible = True  # Class::Enum
            m.lua_type = f'{c.name}.{m.name}'
            m.writable = False
            m.doc = list(m.call.doc)
            if any(v is None for _, v in vals):
                self.unresolve(c, m, 'enum table values not computed')
            return
        res = self.r.resolve(getter, m) if getter else None
        if res is None:
            self.unresolve(c, m, 'getter not resolved: ' + (join_tokens(getter)[:80] if getter else ''))
            m.lua_type = 'any'
            m.doc = list(m.call.doc)
            return
        kind, sig = res
        if kind == 'data' and re.sub(r'\s', '', getter_text).split('::')[-1] == m.name:
            m.cpp_visible = True
        if sig.problem:
            self.unresolve(c, m, 'getter: ' + sig.problem)
        if sig.cfunc:
            self.unresolve(c, m, 'lua_CFunction property getter')
            m.lua_type = 'any'
            m.doc = list(m.call.doc)
            return
        cpp_type = sig.ret
        m.cpp_type = cpp_type
        if kind in ('member', 'free'):
            n_self = 1 if (kind == 'free' and not static) else 0
            visible = [p for p in sig.params[n_self:] if bare_type(p[0])[0] != 'lua_State']
            if visible:
                self.unresolve(c, m, 'getter takes arguments')
        if cpp_type is None:
            # (an override names the type a lambda's return cannot say)
            if (c.name, m.name) not in PROPERTY_OVERRIDES:
                self.unresolve(c, m, 'getter return type not inferred')
            m.lua_type = 'any'
        else:
            base, stars = bare_type(cpp_type)
            lt = self.tm.lua(cpp_type, c.cpp)
            m.lua_type = lt or 'any'
            m.is_event = re.match(r'^FunctionSubscribe<', base) is not None
            by_value = '&' not in cpp_type and stars == 0 and kind != 'data'
            m.copy = by_value and (lt in self.r.lua_classes or lt.endswith('[]') or lt.startswith('table')) \
                and not m.is_event
        if (c.name, m.name) in PROPERTY_OVERRIDES:
            m.lua_type, m.copy = PROPERTY_OVERRIDES[(c.name, m.name)]
        m.writable = setter is not None
        m.assign_fn = None
        if setter is not None and getattr(m, 'is_event', False):
            sres = self.r.resolve(setter, m)
            if sres is not None and any(bare_type(p[0])[0] == 'lua_State' for p in sres[1].params):
                ev = self.r.lua_classes.get(m.lua_type)
                m.assign_fn = ev
        doc = list(m.call.doc)
        for line in sig.doc:
            if line not in doc:
                doc.append(line)
        if setter is not None and not getattr(m, 'is_event', False):
            sres = self.r.resolve(setter, m)
            if sres is not None and sres[1].problem:
                self.unresolve(c, m, 'setter: ' + sres[1].problem)
            if sres is not None:
                for line in sres[1].doc:
                    if line not in doc:
                        doc.append(line)
        m.doc = doc
        field = HTML_UI_FIELD_OVERRIDES.get((c.name, m.name))
        if field:
            m.lua_type = field.get('type', m.lua_type)
            m.doc = list(field.get('doc', m.doc))

    def unresolve(self, c, m, why):
        m.unresolved.append(why)
        self.unresolved.append(f'{c.name}.{m.name}: {why}')

# The log and the debug overlay (feature debug-overlay on S09's Logger, CoreClassesLua.cpp):
# Log.* take any values (tab-joined) and are lua_CFunctions, as are the buffer's readers and
# the overlay's getStats/addCommand; DebugLogEntry and DebugStats are in
# engine/core/script/binding/stubs/DebugOverlay.lua
for _name, _level in (('print', 'Info'), ('info', 'Info'), ('verbose', 'Trace'), ('debug', 'Debug (compiled out of release builds)'),
                      ('warn', 'Warn'), ('error', 'Error')):
    OVERRIDES[('Log', _name)] = {
        'doc': [f'Logs the values joined with tabs (tostring) at level {_level}, category `lua`, with the calling',
                'chunk and line. Project Settings > Debug > Log Stack Traces decides which levels carry a Lua traceback.'],
        'overloads': [([('...', 'any')], '')]}
OVERRIDES[('Log', 'getEntries')] = {
    'doc': ['The log buffer\'s lines (every record of the Logger), oldest first: the newest `maxCount`',
            '(0 or nil: all) of the `levels` given (a list of LogLevel values; nil: every level).'],
    'overloads': [([('maxCount?', 'integer'), ('levels?', 'integer[]')], 'DebugLogEntry[]')]}
OVERRIDES[('Log', 'count')] = {
    'doc': ['Lines of that LogLevel since the last Log.clear(), buffered or not.'],
    'overloads': [([('level', 'integer')], 'integer')]}
OVERRIDES[('DebugOverlay', 'getStats')] = {
    'doc': ['The stats panel\'s figures now (works while the overlay is off): frame times from the engine\'s',
            'frame-time ring (S15), draw calls, memory, audio, physics, entities, coroutines, timers.'],
    'overloads': [([], 'DebugStats')]}
OVERRIDES[('DebugOverlay', 'addCommand')] = {
    'doc': ['Adds a console command (name `[a-z0-9_-]+`; re-adding replaces): `fn(args)` gets the words after',
            'the name. It goes with `owner` (default: the `self` of the calling script method). False for a bad',
            'or built-in name.'],
    'overloads': [([('name', 'string'), ('description', 'string?'), ('fn', 'fun(args: string[])'), ('owner?', 'table')], 'boolean')]}
