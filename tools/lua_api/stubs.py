# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The LuaLS (EmmyLua) annotation stubs of the engine's Lua API, from lua-api.json only
(emitter 1 of 3, S23 R29): one file per Lua class (`Vector3.lua`, `Sprite.lua`...), one per enum
namespace (`CameraType.lua`), `Globals.lua` (RegisterEngineEvent, RegisterEvent, NULL_ENTITY,
event objects), `Script.lua` (the script module contract: ScriptInstance, properties, onReload),
`EditorScript.lua` (editor scripts: the Editor table), plus the lua-language-server plugin that has
assignments to the classes' static fields through the global table checked (LUALS_PLUGIN).

Types the parser could not resolve are `any` (never a wrong type), and the binding is listed with
`-- unresolved:` in the stub, which tools/mcp/tests/test_stubs.py checks stays empty.
"""

import os

FILE_HEADER = ('---@meta _\n'
               '-- JasperEngine Lua API stubs (LuaLS / EmmyLua annotations), generated from the engine\'s\n'
               '-- LuaBridge bindings by the editor. Do not edit: the editor rewrites this directory.\n')


# The lua-language-server plugin written next to the stubs (.luarc.json `runtime.plugin`).
# LuaLS (3.x, vm/global.lua) compiles an assignment to a field of a global name,
# `Engine.timeScale = v`, as a set of the global variable `Engine.timeScale` and types it by the
# assigned value (vm.compileByGlobal), so the `---@field timeScale number` of the class is never
# compared with `v` and a wrong type passes. A prefix that is not a global name, `(Engine).timeScale`
# or a local alias, is checked against the field like `obj.field = v`. Before the file is compiled
# (OnTransformAst) the plugin wraps the prefix of every assignment to a static field the stubs
# declare in such a parenthesis node, in place: no text changes, positions stay.
LUALS_PLUGIN_NAME = 'luals-plugin.lua.txt'
LUALS_PLUGIN = r"""-- JasperEngine: lua-language-server plugin (`runtime.plugin` in the project's .luarc.json),
-- generated with the Lua API stubs (lua/) by the editor. Do not edit: the editor rewrites it.
--
-- lua-language-server reads `Engine.timeScale = v`, an assignment to a field of a global name,
-- as the definition of a global `Engine.timeScale` typed by `v`, so a wrong type is not reported
-- there (it is through an object or a local: `local e = Engine; e.timeScale = "x"`). For the
-- static fields of the engine classes listed below, this plugin makes the server read such an
-- assignment as `(Engine).timeScale = v`, which it checks against the field's `---@field` type.
-- Only the syntax tree changes: the text, positions and messages are the server's own. Other
-- fields (a game's own `Engine.myFlag = true`) and the game's own globals are left alone.

---@type table<string, string> class -> its static fields, space-separated
local STATIC_FIELDS = {
@FIELDS@
}

local fields = {}
for class, names in pairs(STATIC_FIELDS) do
    local set = {}
    for name in names:gmatch('%S+') do
        set[name] = true
    end
    fields[class] = set
end

local okGuide, guide = pcall(require, 'parser.guide')

local function transform(ast)
    -- The tree is walked with guide.eachChild, not guide.eachSource(Type): those cache their
    -- source lists in the tree, and this runs before the server adds the doc comments to it.
    local found = {}
    local stack = { ast }
    while #stack > 0 do
        local source = stack[#stack]
        stack[#stack] = nil
        if source.type == 'setfield' or source.type == 'setindex' then
            found[#found + 1] = source
        end
        guide.eachChild(source, function (child)
            stack[#stack + 1] = child
        end)
    end
    for _, source in ipairs(found) do
        local node = source.node
        -- a global name of the chunk's environment (not a local, not a field)
        if node and node.type == 'getglobal' and node.next == source
        and node.node and node.node.tag == '_ENV' then
            local known = fields[node[1]]
            local key = guide.getKeyName(source)
            if known and type(key) == 'string' and known[key] then
                -- what the parser builds for `(Engine).timeScale = v`
                local paren = {
                    type   = 'paren',
                    start  = node.start,
                    finish = node.finish,
                    exp    = node,
                    parent = source,
                    next   = source,
                }
                node.parent = paren
                node.next   = nil
                source.node = paren
            end
        end
    end
end

function OnTransformAst(uri, ast)
    if okGuide and type(ast) == 'table' then
        -- a server whose syntax tree differs: no checks rather than an error
        pcall(transform, ast)
    end
    return nil -- changed in place
end

-- With several plugins (this one first in runtime.plugin), the server stops at the first one
-- without the event and keeps the last one's result: pass the other events on.
function OnSetText(uri, text)
    return nil
end

function ResolveRequire(uri, name)
    return nil
end
"""


def luals_plugin(static_fields):
    rows = []
    for cls, names in sorted(static_fields.items()):
        line = f'    {cls} = "{" ".join(names)}",'
        if len(line) > 100:
            words, cur = [], '       '
            for n in names:
                if len(cur) + 1 + len(n) > 100:
                    words.append(cur)
                    cur = '       '
                cur += ' ' + n
            words.append(cur)
            line = f'    {cls} = [[\n' + '\n'.join(words) + '\n    ]],'
        rows.append(line)
    return LUALS_PLUGIN.replace('@FIELDS@', '\n'.join(rows))


def doc_lines(lines, indent=''):
    out = []
    for l in lines:
        if 'LuaBridge' in str(l):
            continue  # notes about the binding mechanics, not the API
        for part in str(l).split('\n'):
            part = part.rstrip()
            if part:
                out.append(f'{indent}---{part}' if part.startswith(' ') else f'{indent}---{part}')
    return out


def c_string_literals(text, max_part=12000):
    """Split text into C string literal parts (each a sequence of adjacent literals)."""
    parts = []
    cur = []
    size = 0
    for line in text.splitlines(keepends=True):
        esc = ''
        for ch in line.encode('utf-8'):
            if ch == 0x5C:
                esc += '\\\\'
            elif ch == 0x22:
                esc += '\\"'
            elif ch == 0x0A:
                esc += '\\n'
            elif ch == 0x09:
                esc += '\\t'
            elif ch == 0x3F:
                esc += '\\?'  # no trigraphs
            elif 0x20 <= ch < 0x7F:
                esc += chr(ch)
            else:
                esc += '\\%03o' % ch
        if size + len(line) > max_part and cur:
            parts.append(cur)
            cur = []
            size = 0
        cur.append(f'"{esc}"')
        size += len(line)
    if cur:
        parts.append(cur)
    return parts


def write_header(files, path, entity_handle_classes=(), plugin=''):
    out = ['// Auto-generated by generate_lua_stubs.py from the Lua bindings. DO NOT EDIT.',
           '#pragma once', '', 'namespace jasperengine::editor::luastubs {', '',
           'struct StubFile {', '    const char* name;', '    const char* const* parts;', '    unsigned int partCount;', '};', '']
    names = sorted(files)
    for idx, name in enumerate(names):
        parts = c_string_literals(files[name])
        out.append(f'static const char* const file{idx}[] = {{')
        for p in parts:
            out.append('    ' + '\n    '.join(p) + ',')
        out.append('};')
    out.append('')
    out.append('static const StubFile stubFiles[] = {')
    for idx, name in enumerate(names):
        out.append(f'    {{"{name}", file{idx}, sizeof(file{idx}) / sizeof(file{idx}[0])}},')
    out.append('};')
    out.append('')
    out.append(f'// The lua-language-server plugin, written to engine-api/{LUALS_PLUGIN_NAME} (runtime.plugin in .luarc.json)')
    out.append('static const char* const pluginParts[] = {')
    for p in c_string_literals(plugin):
        out.append('    ' + '\n    '.join(p) + ',')
    out.append('};')
    out.append(f'static const StubFile luaLsPlugin = {{"{LUALS_PLUGIN_NAME}", pluginParts, sizeof(pluginParts) / sizeof(pluginParts[0])}};')
    out.append('')
    out.append('// The EntityHandle classes (GameObject, Sprite...): a script property of one of these types')
    out.append('// (case-insensitive) holds that wrapper of the referenced GameObject')
    out.append('static const char* const entityHandleClasses[] = {')
    for name in entity_handle_classes:
        out.append(f'    "{name}",')
    out.append('};')
    out.append('')
    out.append('} // namespace jasperengine::editor::luastubs')
    out.append('')
    content = '\n'.join(out)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    old = None
    if os.path.exists(path):
        with open(path, 'r', encoding='utf-8') as f:
            old = f.read()
    if old != content:
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(content)
    else:
        os.utime(path)  # the build compares its time with the inputs'




def html_ui_types_file(api):
    """HtmlUITypes.lua: the LuaLS shapes of the HTML UI API that no binding declares."""
    L = ['-- HTML UI (UIDocument, UIElement, UIEvent, anime): shared types. Guide: jasper://docs/html-ui', '',
         '---The event names documents dispatch (browser names). A custom name of dispatchEvent needs a',
         '---`---@cast name UIEventType` to pass the check; it works at run time.',
         '---@alias UIEventType']
    for name, what in api.get('htmlUi', {}).get('eventTypes', []):
        L.append(f'---| "{name}" # {what}')
    L += ['',
          '---A finding of UIDocument:getDiagnostics() (the codes: jasper://docs/html-ui-support)',
          '---@class UIDiagnostic',
          '---@field severity "error"|"warning"|"info"',
          '---@field code string # stable, "HTMLUI-CSS-0004"',
          '---@field name string # the code\'s name, "unsupported-value"',
          '---@field message string',
          '---@field suggestion string # the fix, "" when there is none',
          '---@field file string # asset-relative ("ui/menu.css"), "" when generated',
          '---@field line integer # 1-based, 0 = unknown',
          '---@field column integer # 1-based, 0 = unknown',
          '---@field text string # the whole line as the Console prints it',
          '',
          '---An effect of UIElement:getEffects() (the effects: jasper://docs/html-ui-effects)',
          '---@class UIEffectInfo',
          '---@field property "background-image"|"mask-image"|"filter"|"backdrop-filter"',
          '---@field index integer # 1-based: the layer or function in the property',
          '---@field name string # the effect, "frosted-glass"',
          '---@field form "paint"|"filter" # paint: an image layer; filter: in filter, backdrop-filter or gradient text',
          '---@field params table<string, string> # every parameter\'s canonical CSS text, universal ones included',
          '---@field stateful boolean # a simulation (cost class stateful): the fields below describe it',
          '---@field stateBytes integer # its state images\' bytes (0 before its first frame)',
          '---@field steps integer # steps since its reset',
          '---@field simTime number # its simulation time, seconds',
          '---@field paused boolean # not drawn last frame (culled, hidden, removed): it does not step',
          '',
          '---The table of UIElement:effectSplat',
          '---@class UIEffectSplat',
          '---@field x number # effect area CSS px',
          '---@field y number',
          '---@field radius number? # CSS px, default 24',
          '---@field dx number? # velocity, px/s',
          '---@field dy number?',
          '---@field color string? # a CSS colour, default "#ffffff"',
          '---@field strength number? # default 1',
          '---@field effect string? # one effect\'s name; default every stateful effect of the element',
          '',
          '---The table of UIItemsView:setProvider (indices 1-based)',
          '---@class UIItemsProvider',
          '---@field count integer # the items (a tree-view: unused, the roots come from children)',
          '---@field bind fun(row: UIElement, i: integer)? # fills a row element for item i',
          '---@field unbind fun(row: UIElement, i: integer)? # clears it before it shows another item',
          '---@field key fun(i: integer): string|integer? # the item\'s identity (default: its index)',
          '---@field height fun(i: integer): number? # a known height, px (item-height="auto" measures the others)',
          '---@field children fun(key: string|nil): (string|integer)[]? # tree-view: the keys of a node\'s children (nil: the roots)',
          '---@field hasChildren fun(key: string): boolean? # tree-view: whether a node has children',
          '',
          '---The options of UIElement:setTooltip',
          '---@class UITooltipOptions',
          '---@field placement "top"|"bottom"|"left"|"right"|"auto"|nil # default "top"; flipped and clamped to the view',
          '---@field delay number? # seconds before it shows (the document\'s, 0.5 by default)',
          '---@field arrow boolean? # an arrow pointing at the element (.htmlui-tooltip-arrow)',
          '---@field interactive boolean? # the pointer may move onto it without hiding it',
          '',
          '---What a hot reload of an .html document did (doc.onReload, doc.lastReloadReport)',
          '---@class UIReloadReport',
          '---@field mode "patch"|"rebuild"',
          '---@field file string',
          '---@field kept integer # elements as they were',
          '---@field updated integer # elements with an attribute, class, inline style or text the author changed',
          '---@field inserted integer',
          '---@field removed integer',
          '---@field replaced integer # subtrees made anew (a changed element type or data binding)',
          '---@field runtimeKept integer # nodes scripts added that stayed',
          '---@field scriptsRerun integer # <script type="text/lua"> that changed and ran again',
          '---@field fallbackReason string|nil # why a patch was not possible',
          '---@field ms number # the reload, the compile excluded',
          '---@field causes string[] # each replaced element and why']
    return FILE_HEADER + '\n'.join(L) + '\n'


def _sig_fun(s, self_type=None):
    parts = []
    if self_type:
        parts.append(f'self: {self_type}')
    for prm in s['params']:
        parts.append(f'{prm["name"]}{"?" if prm["optional"] else ""}: {prm["type"]}')
    if s.get('vararg'):
        parts.append(f'...: {s["vararg"]}')
    out = f'fun({", ".join(parts)})'
    if s['returns']:
        out += f': {s["returns"][0]}'
    return out


class Emitter:
    """The stub files of an API description (extract.py's JSON): one per class and enum,
    Globals.lua, Script.lua, EditorScript.lua and HtmlUITypes.lua, then the hand-written fragments."""

    def __init__(self, api):
        self.api = api
        self.classes = {c['name']: c for c in api['classes']}
        self.files = {}
        self.static_fields = {}  # class -> its static `---@field` names (the LuaLS plugin's table)
        self.plugin = ''

    def emit_class(self, c):
        L = [FILE_HEADER]
        L += doc_lines(c['doc'] + c['conditions'] + c.get('guide', []))
        L.append(f'---@class {c["name"]}' + (f' : {c["base"]}' if c['base'] else '') if not c['eventObject']
                 else f'---@class {c["name"]} : FunctionSubscribe')
        for s in c['constructors']:
            L.append(f'---@overload {_sig_fun(s)}')
        for m in c['properties']:
            notes = []
            if m['static']:
                notes.append('(static)')
                self.static_fields.setdefault(c['name'], []).append(m['name'])
            if m['readonly']:
                notes.append('(read-only)')
            if m['copy']:
                notes.append('(copy: changing its fields changes nothing, assign a new value)')
            if m['event']:
                handler = m.get('eventHandler')
                notes.append('(event: subscribe a script method with RegisterEvent(self, obj.' + m['name'] + ', "method"), or any '
                             'function with :add(tag, fn)' + ('; assigning a function also adds it' if m.get('eventAssignAdds') else '')
                             + (f'; handler: {handler}' if handler else '') + ')')
            notes += m['conditions']
            desc = ' '.join(notes + [' '.join(m['doc'])]).strip()
            L.append(f'---@field {m["name"]} {m["type"]}' + (f' # {desc}' if desc else ''))
            for u in m['unresolved']:
                L.append(f'-- unresolved: {c["name"]}.{m["name"]}: {u}')
        for s in c['callOverloads']:
            L.append(f'---@overload {_sig_fun(s)}')
        for o in c['operators']:
            if 'operand' in o:
                L.append(f'---@operator {o["op"]}({o["operand"]}): {o["returns"]}')
            else:
                L.append(f'---@operator {o["op"]}: {o["returns"]}')
        L.append(f'{c["name"]} = {{}}')
        L.append('')
        for t in c['enumTables']:
            L.append(f'---Values of `{c["name"]}.{t["name"]}` (integers).')
            L.append(f'---@class {c["name"]}.{t["name"]}')
            for v in t['values']:
                L.append(f'---@field {v["name"]} integer' + (f' # {v["value"]}' if v['value'] is not None else ''))
            L.append('')
        for m in c['methods']:
            sigs = m['overloads']
            sep = '.' if m['static'] else ':'
            doc = list(m['doc'])
            if m['static']:
                doc = doc + ['(static: call with a dot, `' + c['name'] + '.' + m['name'] + '(...)`)']
            doc += m['conditions']
            L += doc_lines(doc)
            if any(d.lower().startswith('deprecated') for d in doc):
                L.append('---@deprecated')   # a binding commented "deprecated: ..." (an old alias)
            if m.get('async'):
                L.append('---@async')   # it yields: only inside an engine coroutine
            for u in m['unresolved']:
                L.append(f'-- unresolved: {c["name"]}.{m["name"]}: {u}')
            if not sigs:
                L.append('---@param ... any')
                L.append('---@return any')
                L.append(f'function {c["name"]}{sep}{m["name"]}(...) end')
                L.append('')
                continue
            first = sigs[0]
            names = []
            for prm in first['params']:
                d = prm.get('description', '')
                L.append(f'---@param {prm["name"]}{"?" if prm["optional"] else ""} {prm["type"]}' + (f' # {d}' if d else ''))
                names.append(prm['name'])
            if first.get('vararg'):
                L.append(f'---@param ... {first["vararg"]}')
                names.append('...')
            if first['returns']:
                L.append(f'---@return {first["returns"][0]}')
            for s in sigs[1:]:
                L.append(f'---@overload {_sig_fun(s, None if m["static"] else c["name"])}')
            L.append(f'function {c["name"]}{sep}{m["name"]}({", ".join(names)}) end')
            L.append('')
        self.files[f'{c["name"]}.lua'] = '\n'.join(L).rstrip() + '\n'

    def emit_enum(self, en):
        L = [FILE_HEADER]
        L += doc_lines(en['doc'] + en['conditions'])
        if en['computed']:
            L.append(f'---@enum {en["name"]}')
            L.append(f'{en["name"]} = {{')
            for v in en['values']:
                for dl in doc_lines(v['doc'], '    '):
                    L.append(dl)
                L.append(f'    {v["name"]} = {v["value"]},')
            L.append('}')
        else:
            # values the generator could not compute: still typed, not numbered
            L.append(f'---@class {en["name"]}')
            for v in en['values']:
                L.append(f'---@field {v["name"]} integer' + (f' # {v["value"]}' if v['value'] is not None else ''))
            L.append(f'{en["name"]} = {{}}')
            L.append(f'-- unresolved: {en["name"]}: enum values not computed')
        self.files[f'{en["name"]}.lua'] = '\n'.join(L).rstrip() + '\n'

    def emit_globals(self):
        L = [FILE_HEADER]
        for g in self.api['globals']:
            L += doc_lines(g['doc'])
            L.append('---@type integer')
            L.append(f'{g["name"]} = {g["value"]}')
            L.append('')
        events = self.api['events']
        L.append('---Names of the Engine events a script can subscribe to with RegisterEngineEvent: the')
        L.append('---script method with that name is called on every event.')
        if events:
            L.append('---@alias EngineEventName')
            for ev in events:
                args = ', '.join(f'{a["name"]}: {a["type"]}' for a in ev['args'])
                L.append(f'---| "{ev["name"]}" # handler: function Script:{ev["name"]}({args})' + (f' -> {ev["returns"]}' if ev['returns'] else ''))
        else:
            L.append('---@alias EngineEventName string')
        L.append('')
        L += [
            '---Subscribes `self:<methodName>(...)` to the Engine event of the same name (`Engine.<methodName>`):',
            '---`RegisterEngineEvent(self, "onUpdate")` in `init()` makes the engine call `self:onUpdate()` every',
            '---frame. The method must already exist on `self`. Registering the same method again (same tag)',
            '---replaces the subscription; subscriptions end when the script instance is released. Engine events',
            '---are skipped while the script\'s GameObject is inactive or the script is disabled.',
            '---@param self table # the script instance (`self`)',
            '---@param methodName EngineEventName',
            '---@param tag? string # subscription tag (default: unique per instance and method)',
            'function RegisterEngineEvent(self, methodName, tag) end',
            '',
            '---Subscribes `self:<methodName>(...)` to any event object: a component or physics event such as',
            '---`self.scene:getPhysicsSystem().beginContact2D` or `Button(self.scene, self.entity):getButtonComponent().onPress`.',
            '---Not guarded by the GameObject\'s active state.',
            '---@param self table # the script instance (`self`)',
            '---@param event FunctionSubscribe|function # the event object (an event field: its type also lists the function it accepts)',
            '---@param methodName string',
            '---@param tag? string',
            'function RegisterEvent(self, event, methodName, tag) end',
            '',
            '---Unity\'s DontDestroyOnLoad (same as GameObject.dontDestroyOnLoad): the root GameObject of `obj`, with',
            '---its children, scripts\' state, sounds and bodies, survives every SceneManager.loadScene in the',
            '---persistent scene ("DontDestroyOnLoad"). Root GameObjects only (else a warning and false); nil is ignored.',
            '---@param obj EntityHandle|ScriptInstance|nil # a script instance (`self`) or a GameObject',
            '---@return boolean',
            'function DontDestroyOnLoad(obj) end',
            '',
            '---Removes what `RegisterEngineEvent(self, methodName [, tag])` subscribed (same tag); true if it was',
            '---subscribed. Unity\'s `-=` for an engine event, typically in onDisable.',
            '---@param self table # the script instance (`self`)',
            '---@param methodName EngineEventName',
            '---@param tag? string',
            '---@return boolean',
            'function UnregisterEngineEvent(self, methodName, tag) end',
            '',
            '---Removes what `RegisterEvent(self, event, methodName [, tag])` subscribed (same tag); true if it was',
            '---subscribed.',
            '---@param self table # the script instance (`self`)',
            '---@param event FunctionSubscribe|Event',
            '---@param methodName string',
            '---@param tag? string',
            '---@return boolean',
            'function UnregisterEvent(self, event, methodName, tag) end',
            '',
            '---An event object (the `FunctionSubscribe_*` classes): functions subscribed to it are called when',
            '---the event fires. Subscribe a script method with RegisterEvent / RegisterEngineEvent, or any',
            '---function with `event:add(tag, fn)`; `event:remove(tag)` and `event:has(tag)` by tag.',
            '---@class FunctionSubscribe',
            '',
            '---What Input.getTouches() returns for each touch (a plain table, not a class).',
            '---@class Touch',
            '---@field pointer integer',
            '---@field position Vector2',
            '',
        ]
        self.files['Globals.lua'] = '\n'.join(L).rstrip() + '\n'

    def derives(self, name, base):
        seen = set()
        while name is not None and name not in seen:
            if name == base:
                return True
            seen.add(name)
            c = self.classes.get(name)
            name = c['base'] if c else None
        return False

    def entity_handle_classes(self):
        return sorted(c['name'] for c in self.api['classes'] if self.derives(c['name'], 'EntityHandle'))

    def emit_script(self):
        wrappers = self.entity_handle_classes()
        types = ['bool', 'boolean', 'int', 'integer', 'float', 'number', 'string', 'vector2', 'vec2', 'vector3',
                 'vec3', 'vector4', 'vec4', 'color3', 'color4', 'Prefab', 'entity'] + wrappers
        L = [FILE_HEADER]
        L += [
            '-- The script module contract. A Lua script returns a module table; for every Script component',
            '-- entry using it the engine makes an instance (the module is its __index), sets `self.__name`,',
            '-- `self.scene`, `self.entity` and every property, then calls `init(self)`. Annotate a script\'s',
            '-- module as a subclass to get completion and checks on `self`:',
            '--',
            '--   ---@class Player : ScriptInstance',
            '--   ---@field speed number',
            '--   local Player = { properties = { { name = "speed", type = "float", default = 5.0 } } }',
            '--',
            '--   function Player:init() RegisterEngineEvent(self, "onUpdate") end',
            '--   function Player:onUpdate() local dt = Engine.deltatime end',
            '--   return Player',
            '',
            '---Value of a property entry\'s `type` (case-insensitive). Any other string is a Lua script class',
            '---name: the property then holds that GameObject\'s script instance.',
            '---@alias ScriptPropertyType',
        ]
        for t in types:
            L.append(f'---| "{t}"')
        L += [
            '---| string',
            '',
            '---One entry of a script\'s `properties` table: shown in the Inspector and set on `self.<name>`',
            '---before `init()`. Entity references (a wrapper class name, "entity" or a script class name) are',
            '---nil when unassigned; "Prefab" is a PrefabAsset or nil.',
            '---@class ScriptPropertyDef',
            '---@field name string # field name on `self` (required)',
            '---@field displayName? string # Inspector label (default: name)',
            '---@field type? ScriptPropertyType # default "float"',
            '---@field default? any # default value: number, boolean, string, or an array table for vectors/colors ({x, y, z})',
            '',
            '---What a script that changed while the game played gets in `onReload(info)` (Preferences > Scripting',
            '---> Reload And Continue Playing): the new code runs in the same instance, its fields kept.',
            '---@class ScriptReloadInfo',
            '---@field path string # the script file',
            '---@field addedProperties string[] # properties the new code declares (set to their default)',
            '---@field removedProperties string[] # properties it no longer declares (now nil)',
            '---@field changedProperties string[] # properties of another type now (converted, else the default)',
            '---@field droppedCallbacks integer # anonymous callbacks and timers of the old code, no longer called',
            '',
            '---A Lua script instance (`self` in a script\'s methods), Unity\'s MonoBehaviour. Every Script component',
            '---entry has one, enabled or not. The engine calls these methods by name when the script defines',
            '---them (lua-api.md 1.1):',
            '---  init(self)              once, when the GameObject is first active in hierarchy, enabled or not (Awake)',
            '---  onEnable(self)          it becomes active and enabled (after init)',
            '---  start(self)             once, at the start of the frame after the first onEnable (Start)',
            '---  onDisable(self)         it stops being active and enabled; before onDestroy',
            '---  onDestroy(self)         once: destroy, scene unload, Play stop / quit (only if init ran)',
            '---  onApplicationPause(self, paused) / onApplicationFocus(self, focused) / onApplicationQuit(self)',
            '---  onReload(self, info)    after the script\'s file changed while the game plays (init does not run',
            '---                          again; re-create anonymous callbacks there)',
            '---The component UI\'s pointer callbacks (Unity\'s IPointerDownHandler...), e: PointerEventData, sent to',
            '---the element under the pointer or its first ancestor whose script defines them (lua-api.md 1.7):',
            '---  onPointerDown / onPointerUp / onPointerClick / onPointerEnter / onPointerExit(self, e)',
            '---  onInitializePotentialDrag / onBeginDrag / onDrag / onEndDrag / onDrop / onScroll(self, e)',
            '---The component UI\'s selection callbacks (Unity\'s ISelectHandler...), e: BaseEventData (onMove:',
            '---AxisEventData), sent to the selected Button\'s GameObject (onCancel: up its parents to a handler):',
            '---  onSelect / onDeselect / onMove / onSubmit / onCancel(self, e)',
            '---Update methods (onUpdate, onFixedUpdate, onPostUpdate) run only once registered with',
            '---RegisterEngineEvent (those follow a reload by themselves); they are skipped while the script is',
            '---not active and enabled.',
            '---@class ScriptInstance : ScriptCallbacks',
            '---@field scene Scene # the scene of the GameObject the script is attached to',
            '---@field entity integer # the GameObject\'s entity; wrap it: `Sprite(self.scene, self.entity)`',
            '---@field __name string # the script class name (the file name)',
            '---@field properties? ScriptPropertyDef[] # the properties the Inspector shows',
            '---@field onReload? fun(self: ScriptInstance, info: ScriptReloadInfo) # after a hot reload during Play',
            '---@field enabled boolean # Unity\'s enabled: assigning calls onEnable / onDisable (and start on the next frame)',
            '---@field isActiveAndEnabled boolean # (read-only) the GameObject is active in hierarchy and the script enabled',
            '---@field gameObject GameObject # (read-only) the script\'s GameObject',
            'local ScriptInstance = {}',
            '',
            '---Unity\'s StartCoroutine: runs `fn(...)` (or the method `self:<name>(...)`) as an engine coroutine owned',
            '---by this script, now until its first wait (Coroutine.wait, nextFrame, waitUntil, waitFor...). It stops',
            '---when the GameObject is deactivated or destroyed (not when the script is disabled); an error is logged',
            '---with its traceback and the handle gets status "error". Raises on an inactive GameObject.',
            '---@param fnOrMethodName (async fun(...))|string',
            '---@param ... any',
            '---@return Coroutine',
            'function ScriptInstance:startCoroutine(fnOrMethodName, ...) end',
            '',
            '---Unity\'s StopCoroutine: stops that coroutine (it must be this script\'s), or every coroutine of this',
            '---script started with that method name. A running one (itself) stops at its next wait.',
            '---@param coOrMethodName Coroutine|string',
            'function ScriptInstance:stopCoroutine(coOrMethodName) end',
            '',
            '---Unity\'s StopAllCoroutines: every coroutine this script started (no other script\'s).',
            'function ScriptInstance:stopAllCoroutines() end',
            '',
            '---Unity\'s Invoke: calls `fn()` (or `self:<name>()`, looked up when it fires) once after `delay` seconds,',
            '---game time unless `unscaled`. It keeps running while the script is disabled or the GameObject inactive,',
            '---and is cancelled when the GameObject is destroyed or its scene unloads. (Timer.after with',
            '---{owner = self} is the same timer on the global Timer.)',
            '---@param fnOrMethodName fun()|string',
            '---@param delay number',
            '---@param unscaled? boolean',
            '---@return ScriptTimer',
            'function ScriptInstance:invoke(fnOrMethodName, delay, unscaled) end',
            '',
            '---Unity\'s InvokeRepeating: first after `delay`, then every `interval` (> 0) seconds, at most once a frame.',
            '---@param fnOrMethodName fun()|string',
            '---@param delay number',
            '---@param interval number',
            '---@param unscaled? boolean',
            '---@return ScriptTimer',
            'function ScriptInstance:invokeRepeating(fnOrMethodName, delay, interval, unscaled) end',
            '',
            '---Unity\'s CancelInvoke: every timer of this script, those invoking a method name, or one timer.',
            '---@param timerOrMethodName? ScriptTimer|string',
            'function ScriptInstance:cancelInvoke(timerOrMethodName) end',
            '',
            '---Unity\'s IsInvoking: a timer of this script (or of that method, or that timer) is pending.',
            '---@param timerOrMethodName? ScriptTimer|string',
            '---@return boolean',
            'function ScriptInstance:isInvoking(timerOrMethodName) end',
            '',
            '---The physics messages a script receives when it defines them (Unity OnCollisionEnter...): called',
            '---on the main thread after the fixed step that caused them, enabled or not, while the GameObject',
            '---is active (docs: jasper://docs/physics). The collision is seen from this side (`collision.collider`',
            '---and `collision.gameObject` are the other\'s); triggers get the other collider.',
            '---@class ScriptCallbacks',
            'local ScriptCallbacks = {}',
            '',
        ]
        for name, param, ptype, doc in (
                ('onCollisionEnter', 'collision', 'Collision', 'started touching another collider (3D)'),
                ('onCollisionStay', 'collision', 'Collision', 'still touching, once per fixed step (not while asleep)'),
                ('onCollisionExit', 'collision', 'Collision', 'stopped touching (also when the other goes; no contacts then)'),
                ('onTriggerEnter', 'other', 'Collider', 'a collider entered this trigger, or this entered a trigger (3D)'),
                ('onTriggerStay', 'other', 'Collider', 'still overlapping, once per fixed step'),
                ('onTriggerExit', 'other', 'Collider', 'stopped overlapping (also when the other goes)'),
                ('onCollisionEnter2D', 'collision', 'Collision2D', 'started touching another collider (2D)'),
                ('onCollisionStay2D', 'collision', 'Collision2D', 'still touching, once per fixed step while a body is awake'),
                ('onCollisionExit2D', 'collision', 'Collision2D', 'stopped touching (also when the other goes)'),
                ('onTriggerEnter2D', 'other', 'Collider2D', 'a collider entered this trigger, or this entered a trigger (2D)'),
                ('onTriggerStay2D', 'other', 'Collider2D', 'still overlapping, once per fixed step'),
                ('onTriggerExit2D', 'other', 'Collider2D', 'stopped overlapping (also when the other goes)'),
                ('onJointBreak2D', 'joint', 'Joint2D', 'a 2D joint of this GameObject (or whose bodyA is it) broke')):
            L.append('---' + doc[0].upper() + doc[1:] + '.')
            L.append(f'---@param {param} {ptype}')
            L.append(f'function ScriptCallbacks:{name}({param}) end')
            L.append('')
        # the getScript family on a script instance, as on every GameObject handle (lua-api.json's
        # EntityHandle methods: the emitters read only the JSON)
        handle = self.classes.get('EntityHandle')
        for m in (handle['methods'] if handle else []):
            if not m['name'].startswith('getScript') or not m['overloads']:
                continue
            L += doc_lines(list(m['doc']))
            sig = m['overloads'][0]
            for prm in sig['params']:
                L.append(f'---@param {prm["name"]}' + ('?' if prm['optional'] else '') + f' {prm["type"]}')
            L.append(f'---@return {sig["returns"][0] if sig["returns"] else "any"}')
            L.append(f'function ScriptInstance:{m["name"]}(' + ', '.join(prm['name'] for prm in sig['params']) + ') end')
            L.append('')
        self.files['Script.lua'] = '\n'.join(L).rstrip() + '\n'

    def emit_editor_script(self):
        L = [FILE_HEADER]
        L += [
            '-- Editor scripts: a .lua file under a folder named Editor with annotations on its module',
            '-- functions adds editor menu items (Unity\'s [MenuItem]). The editor finds them by reading the',
            '-- file; a click runs the function in a sandboxed editor state, its edits one undo step:',
            '--',
            '--   local M = {}',
            '--   ---@menu Tools/Snap To Grid [priority=10] [shortcut=Ctrl+Shift+K]',
            '--   function M.snap(ctx) Editor.call("set_transform", {entity = ctx.selection[1], position = {0, 0, 0}}) end',
            '--   ---@validate snap',
            '--   function M.canSnap(ctx) return #ctx.selection > 0 end',
            '--   ---@toolbar Snap [icon=magnet] [tooltip=Snap the selection]',
            '--   function M.snapButton(ctx) M.snap(ctx) end',
            '--   return M',
            '--',
            '-- Paths: Tools/... (any top-level name) the main menu; GameObject/... also the Hierarchy context',
            '-- menu; Assets/... the Project context menu; CONTEXT/<Component>/... that component\'s Inspector',
            '-- menu; SceneView/... the Scene view\'s.',
            '',
            '---What an editor menu function gets.',
            '---@class EditorMenuContext',
            '---@field menuPath string # the item\'s path, "Tools/Snap To Grid"',
            '---@field selection integer[] # the selected GameObjects of the scene',
            '---@field scene integer # the scene id',
            '---@field assets string[] # project-relative paths (the Project window\'s selection, Assets/... items)',
            '---@field component string # CONTEXT/<Component>/... items: the component',
            '',
            '---What Editor.call returns (and passes to its callback).',
            '---@class EditorToolResult',
            '---@field ok boolean',
            '---@field text string # the tool\'s message',
            '---@field data table|nil # the tool\'s structured result',
            '',
            '---The editor, for editor scripts only (not available to the game).',
            '---@class Editor',
            'Editor = {}',
            '',
            '---Runs an MCP tool of the editor (the agent tools: set_transform, create_entity, select_entities ...;',
            '---jasper://docs/mcp-tools lists them). Every edit is part of the menu item\'s undo step. A tool that',
            '---completes later (a build, a wait) needs `callback`.',
            '---@param tool string',
            '---@param args? table',
            '---@param callback? fun(result: EditorToolResult)',
            '---@return EditorToolResult|nil',
            'function Editor.call(tool, args, callback) end',
            '',
            '---The GameObjects selected in the scene now.',
            '---@return integer[]',
            'function Editor.selection() end',
            '',
            '---Selects these GameObjects (an undoable selection step).',
            '---@param ids integer[]',
            '---@return EditorToolResult',
            'function Editor.select(ids) end',
            '',
            '---Writes to the Console (and to run_editor_menu\'s log).',
            '---@param ... any',
            'function Editor.log(...) end',
            '',
        ]
        self.files['EditorScript.lua'] = '\n'.join(L).rstrip() + '\n'

    def emit_all(self):
        for en in self.api['enums']:
            self.emit_enum(en)
        for c in self.api['classes']:
            self.emit_class(c)
        self.emit_globals()
        self.emit_script()
        self.emit_editor_script()
        self.files['HtmlUITypes.lua'] = html_ui_types_file(self.api)
        self.plugin = luals_plugin(self.static_fields)
        self.add_fragments()
        return self.files

    def add_fragments(self):
        """Hand-written LuaLS files (binding/stubs/*.lua, carried by the JSON: `---@class` shapes of
        parameter tables and what the bindings cannot describe) join the generated ones; a fragment
        named like a generated file is appended to it."""
        for fn in sorted(self.api.get('stubFragments', {})):
            text = self.api['stubFragments'][fn]
            body = text[len('---@meta _\n'):] if text.startswith('---@meta _\n') else text
            if fn in self.files:
                self.files[fn] = self.files[fn].rstrip('\n') + '\n\n' + body.lstrip('\n')
            else:
                self.files[fn] = FILE_HEADER + body.lstrip('\n')


def write_stubs(api, header_path, out_dir=None):
    """The editor's embedded stubs header and, with out_dir, the .lua files and the plugin."""
    emitter = Emitter(api)
    files = emitter.emit_all()
    write_header(files, header_path, emitter.entity_handle_classes(), emitter.plugin)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        for fn in os.listdir(out_dir):
            if fn.endswith('.lua') and fn not in files:
                os.remove(os.path.join(out_dir, fn))
        for fn, text in files.items():
            path = os.path.join(out_dir, fn)
            old = None
            if os.path.exists(path):
                with open(path, 'r', encoding='utf-8') as f:
                    old = f.read()
            if old != text:
                with open(path, 'w', encoding='utf-8', newline='\n') as f:
                    f.write(text)
        with open(os.path.join(out_dir, LUALS_PLUGIN_NAME), 'w', encoding='utf-8', newline='\n') as f:
            f.write(emitter.plugin)
    return files, emitter
