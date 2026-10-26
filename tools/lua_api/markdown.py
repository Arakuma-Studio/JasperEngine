# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The Lua API reference in Markdown, from lua-api.json (emitter 3 of 3, S23 R29): MCP
`doc lua-api-reference` serves it. One section per class (base, constructors, properties,
methods), then the enums, the globals and the Engine events."""


def _sig(name, sep, cls, o):
    params = ', '.join(f'{q["name"]}{"?" if q["optional"] else ""}: {q["type"]}' for q in o['params'])
    if o.get('vararg'):
        params = (params + ', ' if params else '') + f'...: {o["vararg"]}'
    out = f'{cls}{sep}{name}({params})'
    if o['returns']:
        out += f' -> {o["returns"][0]}'
    return out


def _doc(lines):
    text = ' '.join(str(line).strip() for line in lines if 'LuaBridge' not in str(line)).strip()
    return text


def _cell(text):
    return text.replace('|', '\\|').replace('\n', ' ')


def reference(api):
    L = ['# Lua API reference', '',
         'Generated from the engine\'s Lua bindings (lua-api.json, `jasper://lua-api`). Calls with a colon',
         'are methods (`obj:method()`), with a dot static functions (`Class.fn()`); properties are fields',
         '(`obj.prop`). A property marked *copy* returns a new value: assign the whole value to change it.', '']
    classes = sorted(api['classes'], key=lambda c: c['name'].lower())
    L.append('## Classes')
    L.append('')
    L.append(', '.join(f'[{c["name"]}](#{c["name"].lower()})' for c in classes if not c['eventObject']))
    L.append('')
    for c in classes:
        if c['eventObject']:
            continue
        L.append(f'### {c["name"]}')
        L.append('')
        meta = []
        if c['base']:
            meta.append(f'Inherits **{c["base"]}**.')
        meta += c['conditions']
        doc = _doc(c['doc'] + c.get('guide', []))
        if doc:
            L.append(doc)
            L.append('')
        if meta:
            L.append(' '.join(meta))
            L.append('')
        if c['constructors']:
            L.append('Constructors:')
            L.append('')
            for s in c['constructors']:
                L.append(f'- `{_sig("", "", c["name"], s)}`')
            L.append('')
        if c['properties']:
            L.append('| Property | Type | |')
            L.append('|---|---|---|')
            for p in c['properties']:
                notes = []
                if p['static']:
                    notes.append('static')
                if p['readonly']:
                    notes.append('read-only')
                if p['copy']:
                    notes.append('copy')
                if p['event']:
                    notes.append('event')
                text = ', '.join(notes)
                doc = _doc(p['doc'])
                if doc:
                    text = (text + ': ' if text else '') + doc
                L.append(f'| `{p["name"]}` | `{_cell(p["type"])}` | {_cell(text)} |')
            L.append('')
        for m in c['methods']:
            sep = '.' if m['static'] else ':'
            sigs = [f'`{_sig(m["name"], sep, c["name"], o)}`' for o in m['overloads']] or [f'`{c["name"]}{sep}{m["name"]}(...)`']
            line = '- ' + ' / '.join(sigs)
            doc = _doc(m['doc'])
            if doc:
                line += f': {doc}'
            L.append(line)
        if c['methods']:
            L.append('')
    L.append('## Enums')
    L.append('')
    for en in sorted(api['enums'], key=lambda e: e['name'].lower()):
        values = ', '.join(f'`{v["name"]}`' + (f' = {v["value"]}' if v['value'] is not None else '') for v in en['values'])
        doc = _doc(en['doc'])
        L.append(f'- **{en["name"]}**' + (f': {doc}' if doc else '') + f' ({values})')
    L.append('')
    if api['globals']:
        L.append('## Globals')
        L.append('')
        for g in api['globals']:
            doc = _doc(g['doc'])
            L.append(f'- `{g["name"]}` = {g["value"]}' + (f': {doc}' if doc else ''))
        L.append('')
    if api['events']:
        L.append('## Engine events')
        L.append('')
        L.append('`RegisterEngineEvent(self, "<name>")` calls the script method of that name:')
        L.append('')
        for ev in api['events']:
            args = ', '.join(f'{a["name"]}: {a["type"]}' for a in ev['args'])
            L.append(f'- `{ev["name"]}({args})`' + (f' -> {ev["returns"]}' if ev['returns'] else ''))
        L.append('')
    return '\n'.join(L).rstrip() + '\n'
