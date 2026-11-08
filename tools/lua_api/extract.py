# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""lua-api.json: the engine's Lua API as data (S23 R29), from the one parser (parser.py).

    {format: "jasper-lua-api", formatVersion: 1, engineVersion, hash,
     classes: [{name, cpp, base, file, line, doc, conditions, modules, eventObject, constructors[],
                methods: [{name, static, overloads: [{params: [{name, type, optional, description?}],
                           returns: [], vararg?, cpp?}], doc, conditions, modules, unresolved}],
                properties: [{name, type, readonly, static, copy, event, eventHandler?, eventAssignAdds?,
                              doc, conditions, modules, cpp?, unresolved}],
                operators: [{op, operand?, returns}], callOverloads: [], enumTables: [], cppOnlyMethods: []}],
     enums: [{name, doc, conditions, modules, computed, values: [{name, value, doc}]}],
     globals: [{name, value, doc}], events: [{name, args: [{name, type}], returns}],
     unresolved: [], htmlUi: {eventTypes: [[name, what]]}, stubFragments: {file: text},
     manualSymbols: [{parent, name, kind, detail}]}

formatVersion follows SemVer: added keys keep 1, readers ignore keys they do not know. Every
list is in the bindings' order, so two extractions of one tree are byte-identical.

`modules` names the optional engine modules a binding is compiled under (the #if of
JASPERENGINE_HTMLUI, JASPERENGINE_VIDEO, JASPERENGINE_PHYSICS_2D/3D around it): the export finds
what a project's scripts use from these, so a binding added under a module's condition is
detected with no list to update (docs/specs/S34-player-size.md D4).
"""

import hashlib
import json
import os
import re

from . import parser as p
from .manual import MANUAL_SYMBOLS

FORMAT = 'jasper-lua-api'
FORMAT_VERSION = 1


def _cond_notes(cond):
    notes = []
    for cnd in cond or []:
        if cnd in p._PHYSICS_NOTES and p._PHYSICS_NOTES[cnd] not in notes:
            notes.append(p._PHYSICS_NOTES[cnd])
    return notes


# the preprocessor condition of each optional module (docs/specs/S34-player-size.md R1), in the
# forms the bindings use (#if X, #ifdef X, #if defined(X))
_MODULE_MACROS = {
    'JASPERENGINE_HTMLUI': 'htmlui',
    'JASPERENGINE_VIDEO': 'video',
    'JASPERENGINE_PHYSICS_2D': 'physics2d',
    'JASPERENGINE_PHYSICS_3D': 'physics3d',
    'JASPERENGINE_SOUND_LAYER': 'soundlayer',
    'JASPERENGINE_TERRAIN_WORLD': 'terrainworld',
}


def _modules(cond):
    """The modules a condition stack requires: a label that is one of their macros (not an #else
    branch, not a negation)."""
    out = []
    for cnd in cond or []:
        label = cnd.strip()
        if label.startswith('defined(') and label.endswith(')'):
            label = label[len('defined('):-1].strip()
        module = _MODULE_MACROS.get(label)
        if module and module not in out:
            out.append(module)
    return out


def _param(entry):
    name, lua_type, optional = entry[0], entry[1], entry[2]
    out = {'name': name, 'type': lua_type, 'optional': bool(optional)}
    if len(entry) > 3 and entry[3]:
        out['description'] = entry[3]
    return out


def _sig(s, cpp=None):
    out = {'params': [_param(x) for x in s.params], 'returns': [s.ret] if s.ret else []}
    if s.vararg:
        out['vararg'] = s.vararg
    if cpp is not None:
        params, ret = cpp[0], cpp[1]
        defaults = cpp[2] if len(cpp) > 2 else [None] * len(params)
        cpp_params = []
        for (t, n), d in zip(params, defaults):
            param = {'type': p.normalize_type(t), 'name': n or ''}
            if d is not None:
                param['default'] = d  # the C++ default argument (a direct binding: Lua passes nil)
            cpp_params.append(param)
        out['cpp'] = {'params': cpp_params, 'returns': p.normalize_type(ret) if ret else ''}
    return out


def _relative(path, root):
    try:
        return os.path.relpath(path, root).replace(os.sep, '/')
    except ValueError:
        return path.replace(os.sep, '/')


def _class(bd, c, repo_root):
    cls_doc = list(c.doc)
    if c.cpp_class is not None:
        for line in c.cpp_class.doc:
            if line not in cls_doc:
                cls_doc.append(line)
    conditions = _cond_notes(c.cond)
    # the guide links of the HTML UI classes, after the build conditions
    guide = [line for line in p.HTML_UI_CLASS_DOCS.get(c.name, []) if line not in cls_doc + conditions]
    out = {
        'name': c.name,
        'cpp': c.cpp,
        'base': c.base,
        'file': _relative(c.call.src.path, repo_root),
        'line': c.call.line,
        'doc': cls_doc,
        'conditions': conditions,
        'modules': _modules(c.cond),
        'guide': guide,
        'eventObject': c.name.startswith('FunctionSubscribe_'),
        'constructors': [_sig(s) for s in c.lua_ctors],
    }

    # properties: the last registration of a name wins (LuaBridge), in first-seen order
    props = [m for m in c.members if m.kind in ('Property', 'StaticProperty')]
    last = {}
    for m in props:
        last[m.name] = m
    seen = set()
    properties = []
    for m in props:
        if last[m.name] is not m or m.name in seen:
            continue
        seen.add(m.name)
        t = m.lua_type
        handler = None
        ev = bd.r.lua_classes.get(t) if getattr(m, 'is_event', False) else None
        if ev is not None and getattr(ev, 'subscribe_sig', None):
            hp, hr = ev.subscribe_sig
            # an event field's handler arguments by name, where the C++ signature has none
            names = p.EVENT_FIELD_ARGS.get((c.name, m.name))
            if names and len(names) == len(hp):
                hp = [(names[i], tt, o) for i, (_, tt, o) in enumerate(hp)]
            handler = 'fun(' + ', '.join(f'{n}: {tt}' for n, tt, _ in hp) + ')' + (f': {hr}' if hr else '')
            if getattr(m, 'assign_fn', None):
                # assigning a function subscribes it ("luaFunction" tag)
                t = f'{t}|' + handler
        prop = {'name': m.name, 'type': t, 'readonly': not m.writable, 'static': m.kind == 'StaticProperty',
                'copy': bool(getattr(m, 'copy', False)), 'event': bool(getattr(m, 'is_event', False)),
                'doc': list(m.doc), 'conditions': _cond_notes(m.call.cond), 'modules': _modules(m.call.cond),
                'unresolved': list(m.unresolved)}
        if handler:
            prop['eventHandler'] = handler
        if m.kind == 'StaticProperty' and not getattr(m, 'is_event', False) and not getattr(m, 'cpp_visible', False):
            prop['luaOnly'] = True  # no Class::name in C++ (an accessor or a lambda backs it)
        if getattr(m, 'assign_fn', None):
            prop['eventAssignAdds'] = True
        cpp_type = getattr(m, 'cpp_type', None)
        if cpp_type:
            prop['cpp'] = p.normalize_type(cpp_type)
        properties.append(prop)
    out['properties'] = properties
    out['enumTables'] = [{'name': m.name, 'values': [{'name': vn, 'value': vv} for vn, vv in m.nested_enum]}
                         for m in props if getattr(m, 'nested_enum', None)]

    # operators and the call metamethod, in registration order
    operators = []
    call_overloads = []
    for m in c.members:
        if m.kind != 'Method' or not m.name.startswith('__'):
            continue
        if m.name == '__call':
            call_overloads += [_sig(s) for s in getattr(m, 'lua_sigs', [])]
            continue
        op = p._OPERATORS.get(m.name)
        if not op:
            continue
        for s in getattr(m, 'lua_sigs', []):
            if op in ('unm', 'len', 'bnot'):
                operators.append({'op': op, 'returns': s.ret or 'any'})
            elif s.params:
                operators.append({'op': op, 'operand': s.params[0][1], 'returns': s.ret or 'any'})
    out['operators'] = operators
    out['callOverloads'] = call_overloads

    # methods: the last registration of a name wins
    emitted = {}
    for m in c.members:
        if m.kind in ('Method', 'StaticMethod') and not m.name.startswith('__'):
            emitted[m.name] = m
    # the C++ methods of the class and its bases (None: a Lua-only class, nothing to mark)
    cpp_names = None
    if c.cpp_class is not None:
        cpp_names = set()
        for k in bd.model.mro(c.cpp_class):
            cpp_names |= set(k.methods)
    methods = []
    for m in c.members:
        if m.kind not in ('Method', 'StaticMethod') or m.name.startswith('__') or emitted.get(m.name) is not m:
            continue
        sigs = getattr(m, 'lua_sigs', [])
        cpp_sigs = getattr(m, 'cpp_sigs', [])
        overloads = [_sig(s, cpp_sigs[i] if i < len(cpp_sigs) and len(cpp_sigs) == len(sigs) else None) for i, s in enumerate(sigs)]
        method = {'name': m.name, 'static': m.kind == 'StaticMethod', 'overloads': overloads, 'doc': list(m.doc),
                  'conditions': _cond_notes(m.call.cond), 'modules': _modules(m.call.cond),
                  'unresolved': list(m.unresolved)}
        if getattr(m, 'is_async', False):
            method['async'] = True   # it yields the running engine coroutine (LuaLS ---@async)
        if cpp_names is not None and m.name not in cpp_names:
            method['luaOnly'] = True  # bound to a lambda or a helper: C++ has no Class::name
        methods.append(method)
    out['methods'] = methods

    # the public C++ methods LuaBridge does not bind (C++ scripts only)
    bound = {m.name for m in c.members if m.kind in ('Method', 'StaticMethod')}
    cpp_only = []
    if c.cpp_class is not None:
        for name in sorted(c.cpp_class.methods):
            if name in bound or name == c.cpp_class.name or name.startswith('operator') or name.startswith('~'):
                continue
            fns = [f for f in c.cpp_class.methods[name] if not f.ctor]
            if not fns:
                continue
            f = min(fns, key=lambda fn: len(fn.params))
            cpp_only.append({'name': name, 'static': bool(f.static),
                             'params': [{'type': p.normalize_type(x.type), 'name': x.name or ''} for x in f.params],
                             'returns': p.normalize_type(f.ret) if f.ret else ''})
    out['cppOnlyMethods'] = cpp_only
    return out


def _enum(bd, en):
    doc = list(en.doc)
    if en.cpp_enum is not None:
        for line in en.cpp_enum.doc:
            if line not in doc:
                doc.append(line)
    cpp_docs = {n: d for n, _, d in en.cpp_enum.values} if en.cpp_enum is not None else {}
    values = [{'name': n, 'value': v, 'doc': list(call.doc) + list(cpp_docs.get(n, []))} for n, v, call in en.resolved]
    return {'name': en.name, 'doc': doc, 'conditions': _cond_notes(en.call.cond), 'modules': _modules(en.call.cond),
            'computed': all(v is not None for _, v, _ in en.resolved), 'values': values}


def _globals(bd):
    out = []
    for name, expr, call in bd.b.globals:
        text = p.join_tokens(expr)
        value = 0 if 'NULL_ENTITY' in text else p.eval_int(expr, {}, bd.model)
        out.append({'name': name, 'value': value if value is not None else 0, 'doc': list(call.doc)})
    return out


def _events(bd):
    events = []
    engine = bd.r.lua_classes.get('Engine')
    if engine is None:
        return events
    for m in engine.members:
        if m.kind != 'StaticProperty' or not getattr(m, 'is_event', False):
            continue
        ev = bd.r.lua_classes.get(m.lua_type)
        hp, hr = getattr(ev, 'subscribe_sig', ([], '')) if ev else ([], '')
        names = p.ENGINE_EVENT_ARGS.get(m.name, [])
        args = [{'name': names[i] if i < len(names) else n, 'type': t} for i, (n, t, _) in enumerate(hp)]
        events.append({'name': m.name, 'args': args, 'returns': hr or ''})
    return events


def _fragments(binding_dir):
    out = {}
    fragments_dir = os.path.join(binding_dir, 'stubs')
    if not os.path.isdir(fragments_dir):
        return out
    for fn in sorted(os.listdir(fragments_dir)):
        if fn.endswith('.lua'):
            with open(os.path.join(fragments_dir, fn), 'r', encoding='utf-8') as f:
                out[fn] = f.read().replace('\r\n', '\n')
    return out


def build(binding_dir, core_dir):
    """The parser's model of the bindings (parser.Builder, built)."""
    model = p.CppModel()
    for root, dirs, fnames in os.walk(core_dir):
        dirs.sort()
        for fn in sorted(fnames):
            if fn.endswith('.h'):
                model.parse_file(os.path.join(root, fn))
    bindings = p.Bindings(model)
    for fn in sorted(os.listdir(binding_dir)):
        if fn.endswith('.cpp'):
            bindings.parse_file(os.path.join(binding_dir, fn))
    builder = p.Builder(model, bindings)
    builder.build()
    return builder


def extract(binding_dir, core_dir, engine_version='unknown', repo_root=None):
    repo_root = repo_root or os.path.abspath(os.path.join(binding_dir, '..', '..', '..', '..'))
    bd = build(binding_dir, core_dir)
    enums = [_enum(bd, en) for en in bd.b.enums]
    unresolved = list(bd.unresolved)
    for en in enums:
        if not en['computed']:
            unresolved.append(f'{en["name"]}: enum values not computed')
    api = {
        'format': FORMAT,
        'formatVersion': FORMAT_VERSION,
        'engineVersion': engine_version,
        'hash': '',
        'classes': [_class(bd, c, repo_root) for c in bd.b.classes],
        'enums': enums,
        'globals': _globals(bd),
        'events': _events(bd),
        'unresolved': unresolved,
        'htmlUi': {'eventTypes': [list(t) for t in p.HTML_UI_EVENT_TYPES]},
        'stubFragments': _fragments(binding_dir),
        'manualSymbols': [{'parent': a, 'name': b, 'kind': k, 'detail': d} for a, b, k, d in MANUAL_SYMBOLS],
    }
    api['hash'] = content_hash(api)
    return api


def content_hash(api):
    """SHA-256 of the JSON without its hash and version (what changes with the API only)"""
    body = dict(api)
    body.pop('hash', None)
    body.pop('engineVersion', None)
    return hashlib.sha256(json.dumps(body, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode('utf-8')).hexdigest()


def dumps(api):
    return json.dumps(api, indent=1, ensure_ascii=False) + '\n'


def write_if_changed(path, text):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    old = None
    if os.path.exists(path):
        with open(path, 'r', encoding='utf-8') as f:
            old = f.read()
    if old != text:
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    else:
        os.utime(path)  # the build compares its time with the inputs'


def load(path):
    with open(path, 'r', encoding='utf-8') as f:
        api = json.load(f)
    if api.get('format') != FORMAT:
        raise ValueError(f'{path} is not a {FORMAT} file')
    if int(api.get('formatVersion', 0)) != FORMAT_VERSION:
        raise ValueError(f'{path}: formatVersion {api.get("formatVersion")} (this reader knows {FORMAT_VERSION})')
    return api
