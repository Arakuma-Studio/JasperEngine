# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Completion details the bindings cannot give (ported from generate_api_suggestions.py's
MANUAL_SYMBOLS): the HTML UI Lua API's lua_CFunctions and lambdas, whose C++ signature says
nothing about the Lua call, and the shapes only the LuaLS stubs describe (UIDataModel,
anime.utils/eases). (parent, name, kind, detail): the detail replaces the one built from the
bindings; a symbol the bindings do not list is added. search_engine_api returns these details.
"""

_ANIME_PARAMS = ('params: properties to animate (opacity, translateX/Y, scale, rotate, any CSS property in camelCase, '
                 '"--custom") with a value, {from=, to=} or keyframes, plus duration, delay, ease ("outCubic", '
                 '"outElastic(1, .6)"), loop, alternate, autoplay, onBegin, onUpdate, onComplete (ms)')
_VOXEL_OPTIONS = '{blend = metres, material = slot, weight = 0-1, falloff = 0-1, amount = 0-1, iterations = 1-8}'
MANUAL_SYMBOLS = [
    ('VoxelVolume', 'dig', 'Method', 'VoxelVolume:dig(shape, ' + _VOXEL_OPTIONS + ') -> VoxelEditReport'),
    ('VoxelVolume', 'fill', 'Method', 'VoxelVolume:fill(shape, ' + _VOXEL_OPTIONS + ') -> VoxelEditReport'),
    ('VoxelVolume', 'smooth', 'Method', 'VoxelVolume:smooth(shape, ' + _VOXEL_OPTIONS + ') -> VoxelEditReport'),
    ('VoxelVolume', 'paint', 'Method', 'VoxelVolume:paint(shape, ' + _VOXEL_OPTIONS + ') -> VoxelEditReport'),
    ('anime', 'animate', 'StaticMethod',
     'anime.animate(targets, params) -> AnimeAnimation; targets: CSS selector, UIElement, table or a list; ' + _ANIME_PARAMS),
    ('anime', 'createTimeline', 'StaticMethod',
     'anime.createTimeline({defaults = params, loop, autoplay, onComplete...}) -> AnimeTimeline; '
     'tl:add(targets, params, position) with position ms, "+=100", "<", "label"'),
    ('anime', 'createTimer', 'StaticMethod',
     'anime.createTimer({duration, loop, onUpdate, onLoop, onComplete...}) -> AnimeTimer (a clock without targets)'),
    ('anime', 'createSpring', 'StaticMethod',
     'anime.createSpring({bounce, duration} or {mass, stiffness, damping, velocity}) -> AnimeSpring; use as params.ease'),
    ('anime', 'spring', 'StaticMethod', 'anime.spring(params) -> AnimeSpring (same as anime.createSpring)'),
    ('anime', 'stagger', 'StaticMethod',
     'anime.stagger(value or {from, to}, {start, from = "first"|"center"|"last"|index, ease, grid, axis}) -> '
     'a per-target value for delay or a property'),
    ('anime', 'cubicBezier', 'StaticMethod', 'anime.cubicBezier(x1, y1, x2, y2) -> AnimeEase'),
    ('anime', 'steps', 'StaticMethod', 'anime.steps(steps, fromStart) -> AnimeEase'),
    ('anime', 'linear', 'StaticMethod', 'anime.linear(...points) -> AnimeEase'),
    ('anime', 'irregular', 'StaticMethod', 'anime.irregular(length, randomness) -> AnimeEase'),
    ('anime', 'utils', 'Property',
     'anime.utils: set(targets, props), get(target, property), remove(targets), random, clamp, round, snap, wrap, '
     'mapRange, lerp, damp, roundPad, padStart, padEnd, shuffle, stagger'),
    ('anime', 'eases', 'Property', 'anime.eases.<name>: linear, in/out/inOut/outIn + Quad Cubic Quart Quint Sine Expo Circ Bounce Back Elastic'),
    ('AnimeTimeline', 'add', 'Method', 'AnimeTimeline:add(targets, params, position) -> AnimeTimeline; or add(timerParams, position)'),
    ('AnimeTimeline', 'set', 'Method', 'AnimeTimeline:set(targets, props, position) -> AnimeTimeline'),
    ('AnimeTimeline', 'call', 'Method', 'AnimeTimeline:call(function(timeline) end, position) -> AnimeTimeline'),
    ('AnimeTimeline', 'label', 'Method', 'AnimeTimeline:label(name, position) -> AnimeTimeline'),
    ('AnimeTimeline', 'sync', 'Method', 'AnimeTimeline:sync(animationOrTimer, position) -> AnimeTimeline'),
    ('AnimeTimeline', 'remove', 'Method', 'AnimeTimeline:remove(targets, property) -> AnimeTimeline'),
    ('AnimeTimer', 'andThen', 'Method', 'AnimeTimer:andThen(function(timer) end) -> AnimeTimer (Anime\'s then())'),
    ('AnimeTimer', 'seek', 'Method', 'AnimeTimer:seek(ms, muteCallbacks) -> self'),
    ('AnimeTimer', 'stretch', 'Method', 'AnimeTimer:stretch(ms) -> self'),
    ('UIElement', 'addEventListener', 'Method',
     'UIElement:addEventListener(type, function(event) end, capture or {capture=, once=}); type: "click", "keydown", '
     '"input", "change", "wheel", "mouseover", "transitionend"... (UIEventType)'),
    ('UIElement', 'removeEventListener', 'Method', 'UIElement:removeEventListener(type, sameFunction, capture)'),
    ('UIElement', 'dispatchEvent', 'Method', 'UIElement:dispatchEvent(type, detail, bubbles) -> bool (false when prevented)'),
    ('UIElement', 'getAttributes', 'Method', 'UIElement:getAttributes() -> table<string, string>'),
    ('UIElement', 'querySelectorAll', 'Method', 'UIElement:querySelectorAll(selector) -> UIElement[] (array, 1-based)'),
    ('UIElement', 'close', 'Method', 'UIElement:close(returnValue) (<dialog>; dispatches close)'),
    ('UIDocument', 'addEventListener', 'Method',
     'UIDocument:addEventListener(type, function(event) end, capture or {capture=, once=}) (a pointer listener makes a '
     'transparent page take the pointer)'),
    ('UIDocument', 'removeEventListener', 'Method', 'UIDocument:removeEventListener(type, sameFunction, capture)'),
    ('UIDocument', 'querySelectorAll', 'Method', 'UIDocument:querySelectorAll(selector) -> UIElement[] (array, 1-based)'),
    ('UIDocument', 'createDataModel', 'Method',
     'UIDocument:createDataModel(name, {field = value, fn = function(event, ...) end}) -> UIDataModel proxy '
     '(model.field = v updates {{ field }})'),
    ('UIDocument', 'getDataModel', 'Method', 'UIDocument:getDataModel(name) -> UIDataModel or nil'),
    ('UIDocument', 'getDiagnostics', 'Method',
     'UIDocument:getDiagnostics() -> {{severity, code, name, message, suggestion, file, line, column, text}, ...}'),
    ('UIDocument', 'getTargetTexture', 'Method', 'UIDocument:getTargetTexture() -> Framebuffer or nil (TEXTURE render mode)'),
    ('UIDocument', 'getLiveLuaRefCount', 'StaticMethod', 'UIDocument.getLiveLuaRefCount() -> integer'),
    ('UIClassList', 'add', 'Method', 'UIClassList:add(name, ...) (el.classList:add("open"))'),
    ('UIClassList', 'remove', 'Method', 'UIClassList:remove(name, ...)'),
    ('UIClassList', 'toggle', 'Method', 'UIClassList:toggle(name, force) -> bool'),
    ('UIClassList', 'contains', 'Method', 'UIClassList:contains(name) -> bool'),
    ('UIStyle', 'getPropertyValue', 'Method', 'UIStyle:getPropertyValue(name) -> string (computed)'),
    ('UIStyle', 'setProperty', 'Method', 'UIStyle:setProperty(name, value) (inline; "" removes)'),
    ('UIStyle', 'removeProperty', 'Method', 'UIStyle:removeProperty(name) -> string'),
    ('UIDataModel', 'UIDataModel', 'Class', 'class UIDataModel (UIDocument:createDataModel / getDataModel proxy)'),
    ('UIDataModel', 'onChange', 'Method', 'UIDataModel:onChange(name, function(value) end) -> integer'),
    ('UIDataModel', 'removeOnChange', 'Method', 'UIDataModel:removeOnChange(id)'),
    ('UIDataModel', 'dirty', 'Method', 'UIDataModel:dirty(name)'),
    ('UIDataModel', 'dirtyAll', 'Method', 'UIDataModel:dirtyAll()'),
    ('UIDataModel', 'get', 'Method', 'UIDataModel:get("items[0].name") -> any (0-based path)'),
    ('UIDataModel', 'set', 'Method', 'UIDataModel:set("items[0].name", value)'),
    ('UIDataModel', 'getName', 'Method', 'UIDataModel:getName() -> string'),
    ('UIDataModel', 'isValid', 'Method', 'UIDataModel:isValid() -> bool'),
    # Lattice (GameObjectClassesLua.cpp): overloads taking a GameObject or an entity, bound
    # through lambdas for the optional arguments
    ('Lattice', 'bind', 'Method',
     'Lattice:bind(gameObjectOrEntity, weight = 1) -> bool (adds a LatticeModifierComponent binding, '
     'the component when missing)'),
    ('Lattice', 'unbind', 'Method', 'Lattice:unbind(gameObjectOrEntity) -> bool'),
    ('Lattice', 'addFollower', 'Method',
     'Lattice:addFollower(follower, rest, applyPosition = true, applyRotation = true, applyScale = false, '
     'weight = 1) -> bool (GameObjects or entities)'),
    ('Lattice', 'removeFollower', 'Method', 'Lattice:removeFollower(gameObjectOrEntity) -> bool'),
    ('Lattice', 'applyPreset', 'Method',
     'Lattice:applyPreset(name, axis, amount, toward = "") -> bool; name squash|stretch|taper|twist|bend|'
     'bulge|shear|noise, axis/toward "x"|"y"|"z"'),
]
