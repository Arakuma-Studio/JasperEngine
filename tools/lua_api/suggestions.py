# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""engine_api_suggestions.h from lua-api.json (emitter 2 of 3, S23 R29): the code editor's
completion (CustomTextEditor) and MCP search_engine_api (EngineApiContext). Same header API as
the regex parser it replaces (getEngineTypeNames, getEngineEnumNames, getEngineBuiltinNames,
getEngineAPISymbols), the same detail forms:

    class Mesh : GameObject            Vector3(number x, number y, number z)
    Mesh:setColor(Vector4 color)       Engine.getDeltatime() -> float
    GameObject.position -> Vector3 (copy)
    GameObject::getPosition() -> Vector3   (CppMethod: C++ scripts only)
"""

import os


def _cpp_params(params):
    return ', '.join(' '.join(x for x in (q['type'], q.get('name', '')) if x) for q in params)


def _lua_params(params):
    return ', '.join(f'{q["type"]} {q["name"]}' for q in params)


def _method_detail(cls, m):
    sep = '.' if m['static'] else ':'
    overloads = m['overloads']
    if not overloads:
        return f'{cls}{sep}{m["name"]}()'
    with_cpp = [o for o in overloads if o.get('cpp')]
    if with_cpp:
        o = min(with_cpp, key=lambda o: len(o['cpp']['params']))
        detail = f'{cls}{sep}{m["name"]}({_cpp_params(o["cpp"]["params"])})'
        ret = o['cpp']['returns']
        if ret and ret != 'void':
            detail += f' -> {ret}'
        return detail
    o = min(overloads, key=lambda o: len(o['params']))
    detail = f'{cls}{sep}{m["name"]}({_lua_params(o["params"])})'
    if o['returns']:
        detail += f' -> {o["returns"][0]}'
    return detail


def symbols(api):
    """(name, kind, detail, parent) of every completion symbol, in the order the header lists them"""
    enums, members, classes, ctors, methods, statics, props, consts, events, cpp_only = ([] for _ in range(10))
    for en in api['enums']:
        enums.append((en['name'], 'Enum', f'enum {en["name"]}', ''))
        for v in en['values']:
            members.append((v['name'], 'EnumMember', f'{en["name"]}.{v["name"]}', en['name']))
    for c in api['classes']:
        name = c['name']
        classes.append((name, 'Class', f'class {name}' + (f' : {c["base"]}' if c['base'] else ''), ''))
        for s in c['constructors']:
            ctors.append((name, 'Constructor', f'{name}({_lua_params(s["params"])})', name))
        for m in c['methods']:
            (statics if m['static'] else methods).append((m['name'], 'StaticMethod' if m['static'] else 'Method', _method_detail(name, m), name))
        for prp in c['properties']:
            detail = f'{name}.{prp["name"]}'
            if prp['copy']:
                detail += f' -> {prp["type"]} (copy)'
            if prp['static']:
                (events if prp['event'] else consts).append((prp['name'], 'Event' if prp['event'] else 'Constant', detail, name))
            else:
                props.append((prp['name'], 'Property', detail, name))
        # Class.Table enum tables (AABB.BoxType, Plane.Side): a constant of the class
        for t in c['enumTables']:
            consts.append((t['name'], 'Constant', f'{name}.{t["name"]}', name))
        for m in c['cppOnlyMethods']:
            detail = f'{name}::{m["name"]}({_cpp_params(m["params"])})'
            if m['returns'] and m['returns'] != 'void':
                detail += f' -> {m["returns"]}'
            cpp_only.append((m['name'], 'CppMethod', detail, name))

    ordered = enums + members + classes + ctors + methods + statics + props + consts + events + cpp_only
    # the hand-written details replace those of the same symbol, or add the symbol
    by_key = {}
    for i, (n, k, d, parent) in enumerate(ordered):
        by_key.setdefault((parent or n, n, k), []).append(i)
    for s in api.get('manualSymbols', []):
        key = (s['parent'], s['name'], s['kind'])
        if key in by_key:
            for i in by_key[key]:
                n, k, _, parent = ordered[i]
                ordered[i] = (n, k, s['detail'], parent)
        else:
            ordered.append((s['name'], s['kind'], s['detail'], '' if s['kind'] == 'Class' else s['parent']))

    seen = set()
    unique = []
    for sym in ordered:
        key = (sym[0], sym[1], sym[3], sym[2] if sym[1] == 'Constructor' else '')
        if key not in seen:
            seen.add(key)
            unique.append(sym)
    return unique


def lua_only(api):
    """(parent, name, kind) of the symbols C++ has no Class::name for: static properties backed by an
    accessor or a lambda (Engine.deltatime), methods bound to helpers. C++ completion leaves them out."""
    out = set()
    for c in api['classes']:
        for prp in c['properties']:
            if prp.get('luaOnly'):
                out.add((c['name'], prp['name'], 'Event' if prp['event'] else 'Constant'))
        for m in c['methods']:
            if m.get('luaOnly'):
                out.add((c['name'], m['name'], 'StaticMethod' if m['static'] else 'Method'))
    return out


_SUGGESTION_KIND = {'Class': 'Class', 'Constructor': 'Class', 'Enum': 'Enum', 'EnumMember': 'EnumMember', 'Method': 'Method',
                    'CppMethod': 'CppMethod', 'StaticMethod': 'Function', 'Property': 'Property', 'Constant': 'Constant',
                    'Event': 'Property'}


def _escape(s):
    return s.replace('\\', '\\\\').replace('"', '\\"')


def header_text(api):
    syms = symbols(api)
    class_names = sorted({n for n, k, _, _ in syms if k == 'Class'})
    enum_names = sorted({n for n, k, _, _ in syms if k == 'Enum'})
    static_classes = {parent for _, k, _, parent in syms if k == 'StaticMethod'}
    L = ['// Auto-generated by tools/lua_api from lua-api.json - DO NOT EDIT',
         '// The Lua bindings of engine/core/script/binding/*.cpp, through the one API parser',
         '#pragma once', '', '#include <string>', '#include <vector>', '#include <unordered_set>', '',
         'namespace jasperengine::editor {', '']
    for fn, names in (('getEngineTypeNames', class_names), ('getEngineEnumNames', enum_names),
                      ('getEngineBuiltinNames', sorted(static_classes | set(enum_names)))):
        L.append(f'inline std::unordered_set<std::string> {fn}() {{')
        L.append('    return {')
        for n in names:
            L.append(f'        "{_escape(n)}",')
        L.append('    };')
        L.append('}')
        L.append('')
    L += ['struct EngineAPISymbol {', '    const char* name;', '    const char* kind;   // maps to SuggestionKind',
          '    const char* detail;', '    const char* parent; // owning class/enum',
          '    bool luaOnly = false; // no Class::name in C++ (a Lua accessor property, a helper-bound method)', '};', '',
          'inline const std::vector<EngineAPISymbol>& getEngineAPISymbols() {',
          '    static const std::vector<EngineAPISymbol> symbols = {']
    only = lua_only(api)
    for n, k, d, parent in syms:
        flag = ', true' if (parent, n, k) in only else ''
        L.append(f'        {{"{_escape(n)}", "{_SUGGESTION_KIND.get(k, "Variable")}", "{_escape(d)}", "{_escape(parent)}"{flag}}},')
    L += ['    };', '    return symbols;', '}', '', '} // namespace jasperengine::editor', '']
    return '\n'.join(L)


def write_header(api, path):
    text = header_text(api)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    old = None
    if os.path.exists(path):
        with open(path, 'r', encoding='utf-8') as f:
            old = f.read()
    if old != text:
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    else:
        os.utime(path)
