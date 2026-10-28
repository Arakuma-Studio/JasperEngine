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
    __slots__ = ('params', 'ret', 'doc', 'cfunc', 'problem')

    def __init__(self, params, ret, doc=None, cfunc=False, problem=None):
        self.params = params    # [(cpp type, name or None)], Lua-visible only
        self.ret = ret          # cpp type, '' for void, None for unknown
        self.doc = doc or []
        self.cfunc = cfunc
        self.problem = problem  # why the binding is wrong (a cast to a signature that does not exist)

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

    def _parse_chain(self, src, toks, i, chain_doc):
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
            calls.append(Call(name_tok.t, targs, args, name_tok.cond, doc, src, name_tok.line))
            i = k
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
                              substitute(sig.ret, subst) if sig.ret else sig.ret, sig.doc, sig.cfunc)
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
        return Sig(params, fn.ret, fn.doc, cfunc=cfunc)

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

# Properties whose getter returns a LuaRef: (Lua type, a copy)
PROPERTY_OVERRIDES = {
    ('Mesh', 'material'): ('Material', True),
    # AnimationEvent.args builds a 1-based table of the event's arguments
    ('AnimationEvent', 'args'): ('(number|string|boolean)[]', True),
}

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
PROPERTY_OVERRIDES[('UIDocument', 'lastReloadReport')] = ('UIReloadReport|nil', False)

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
# own elements). mouseenter, mouseleave, pointer*, touch*, contextmenu, beforeinput and
# composition* never reach documents (addEventListener warns), so they are not listed.
HTML_UI_EVENT_TYPES = [
    ('click', 'a press and release on the element (Enter/Space on a focused button too)'),
    ('dblclick', 'a double click'),
    ('mousedown', 'a pointer press (event.button: 0 left, 1 middle, 2 right)'),
    ('mouseup', 'a pointer release'),
    ('mousemove', 'the pointer moved over the element'),
    ('mouseover', 'the pointer entered the element or a child (bubbles)'),
    ('mouseout', 'the pointer left the element or a child (bubbles)'),
    ('wheel', 'the mouse wheel (event.deltaX/deltaY in steps, positive down/right)'),
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
            fs = re.match(r'^FunctionSubscribe<(.*?)\((.*)\)>$', c.cpp) or \
                re.match(r'^FunctionSubscribe<(.*?)\((.*)\)>$', c.base_cpp or '')
            for m in c.members:
                if fs and m.kind == 'Method' and m.name in ('call', '__call', 'add'):
                    self.subscribe_member(c, m, fs.group(1), fs.group(2))
                    continue
                if m.kind in ('Method', 'StaticMethod'):
                    self.function_member(c, m)
                else:
                    self.property_member(c, m)

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
        if optional_bools:
            k = len(lp) - 1
            while k >= 0 and lp[k][1] == 'boolean':
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
            cpp_params = sig.params[1:] if drop_self and sig.params else sig.params
            m.cpp_sigs.append(([p for p in cpp_params if bare_type(p[0])[0] != 'lua_State'], sig.ret))
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
        else:
            fn = 'fun(' + ', '.join(f'{n}: {t}' for n, t, _ in handler_params) + ')' + (f': {ret}' if ret else '')
            m.lua_sigs = [LuaSig([('tag', 'string', False), ('fn', fn, False)], 'boolean')]
            m.doc = ['Subscribes `fn` under `tag` (an existing subscription with the same tag is replaced).']
        c.subscribe_sig = (handler_params, ret)

    def handler_arg_type(self, cpp):
        # LuaFunctionBase pushes wchar_t as a UTF-8 string
        if bare_type(cpp)[0] == 'wchar_t':
            return 'string'
        return self.tm.lua(cpp) or 'any'

    def property_member(self, c, m):
        args = m.call.args
        getter = args[1] if len(args) > 1 else None
        setter = args[2] if len(args) > 2 else None
        static = m.kind == 'StaticProperty'
        m.nested_enum = None
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
