#!/usr/bin/env python3
"""Fails when an engine event could outlive Stop: a FunctionSubscribe member left out of its clear.

At Stop the editor unloads the C++ script library, so every callback a script subscribed must be
gone first (Engine::clearAllSubscriptions, Engine::clearComponentSubscriptions, the holders' own
clears), and a hot reload removes them by tag (Scene::removeSubscriptionsByTag,
Engine::removeSubscriptionsByTag). The clears walk lists, and this checks every event of the
engine's sources (engine/, not its libraries, tests or IDE projects). An event is a member of
type FunctionSubscribe<...> or of a class derived from it (Ease, which a script can set to its own
function), alone or in a container (std::array<FunctionSubscribe<...>, N>, an array):

- a holder's instance events are each listed in its forEachEvent: f(member), or the container's
  name (a loop over it);
- a component (engine/core/component/) with events is in ComponentsWithEvents (ComponentEvents.h);
- any other holder is in WALKS below, whose clears at Stop (and on tag removal) call its
  forEachEvent, or in TRANSIENT (its file's values never outlive a call);
- a static event is named in Engine::clearAllSubscriptions and Engine::removeSubscriptionsByTag
  (Engine.cpp): Engine's own by name, any other class's as Class::member;
- an event at namespace scope fails: no clear walks it (make it a static member of a class).

Declarations are read as C++ statements (type arguments across lines, `jasperengine::`
qualification, JASPERENGINE_API, alignas and attributes, several names, `= ...` and `{...}`
initialisers). Locals, parameters, references, pointers, functions and getters are not events.
An event type the lint cannot read where a declaration could be (a macro, a statement it cannot
parse) fails too, so nothing is skipped silently.

    python3 tools/check_event_clears.py [--root DIR] [--self-test]

Prints `file:line: struct.member: <what>` for each problem and exits 1 when there is one.
"""

import argparse
import os
import re
import sys
import tempfile

# the holders outside engine/core/component/ and the functions that walk their forEachEvent:
# each at Stop, before the C++ script library goes, or on a tag removal
WALKS = {
    "PhysicsSystem": [("engine/core/subsystem/PhysicsSystem.cpp", "PhysicsSystem::clearSubscriptions"),
                      ("engine/core/subsystem/PhysicsSystem.cpp", "PhysicsSystem::removeSubscriptionsByTag")],
    # InputActions::unload runs at Stop (editor/Project.cpp) while the Lua state is there
    "InputAction": [("engine/core/input/InputActions.cpp", "InputActions::unload"),
                    ("engine/core/input/InputActions.cpp", "InputActions::resetSession")],
    # a TitleArtRendererComponent's runtime state: Engine::clearComponentSubscriptions and
    # Scene::removeSubscriptionsByTag walk the scene's titles through these
    "TitleArtInstance": [("engine/core/subsystem/TitleArtSystem.cpp", "TitleArtSystem::clearSubscriptions"),
                         ("engine/core/subsystem/TitleArtSystem.cpp", "TitleArtSystem::removeSubscriptionsByTag")],
}
# files whose holders are values that never outlive the call that makes them
TRANSIENT = {
    "engine/core/particles/LegacyParticleSystem.h":
        "the deprecated particle setters' view: built, edited and sampled into curves in one call (ParticleSystem::LegacyEdit)",
}
STATIC_CLEARS = ("Engine::clearAllSubscriptions", "Engine::removeSubscriptionsByTag")
SOURCE_EXTENSIONS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".mm", ".m")
# not engine code: vendored libraries, tests, the IDE projects
SKIPPED_DIRS = ("engine/libs", "engine/tests", "engine/workspaces")
COMPONENT_DIR = "engine/core/component/"
BASE_TYPE = "FunctionSubscribe"

# comments, raw strings, strings and character literals (not a digit separator: 1'000)
LITERAL = re.compile(r"//[^\n]*|/\*.*?\*/|\bR\"([^(\s]*)\(.*?\)\1\"|\"(?:\\.|[^\"\\\n])*\"|"
                     r"(?:(?<!\w)|\b(?:u8|[LuU]))'(?:\\[^'\n]*|[^'\\\n])'", re.S)
PREPROCESSOR = re.compile(r"(?m)^[ \t]*#(?:[^\n\\]|\\\n|\\)*")
# what may stand before a declaration's type: specifiers, export macros, attributes
LEADING = re.compile(r"\s*(?:(static|extern)\b|(?:inline|mutable|constexpr|constinit|thread_local|volatile|const|explicit|virtual)\b"
                     r"|[A-Z][A-Z0-9]*_[A-Z0-9_]*\b|\[\[.*?\]\]|alignas\s*\([^)]*\)|__attribute__\s*\(\(.*?\)\)|__declspec\s*\([^)]*\))",
                     re.S)
ATTRIBUTES = re.compile(r"\[\[.*?\]\]|\balignas\s*\([^)]*\)|__attribute__\s*\(\(.*?\)\)|__declspec\s*\([^)]*\)", re.S)
QUALIFIED_NAME = re.compile(r"\s*(?:::)?\s*(?:\w+\s*::\s*)*(~?\w+)")


def strip_code(text: str) -> str:
    """Comments, string and character literals blanked (newlines kept), so braces in them do not
    count."""
    return LITERAL.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def blank(match):
    return re.sub(r"[^\n]", " ", match.group(0))


def matching(text: str, pos: int, opening: str, closing: str) -> int:
    """The index after the bracket that closes the one at pos (len(text) when none does)."""
    depth = 0
    for i in range(pos, len(text)):
        if text[i] == opening:
            depth += 1
        elif text[i] == closing:
            depth -= 1
            if depth == 0:
                return i + 1
    return len(text)


def without_templates(head: str) -> str:
    """`template<...>` heads removed (their `class T` is not a class)."""
    while True:
        match = re.search(r"\btemplate\s*<", head)
        if not match:
            return head
        head = head[:match.start()] + " " * (matching(head, match.end() - 1, "<", ">") - match.start()) + \
            head[matching(head, match.end() - 1, "<", ">"):]


def struct_name(rest: str) -> str:
    """The name in what follows `struct`/`class`: attributes, alignas and export macros skipped."""
    while True:
        match = LEADING.match(rest)
        if not match or match.end() == 0 or not match.group(0).strip():
            break
        rest = rest[match.end():]
    name = re.match(r"\s*(\w+)", rest)
    return name.group(1) if name and name.group(1) != "final" else "<anonymous>"


def scope_kind(head: str):
    """("namespace" | "struct" | "other", struct name) of the brace a head opens."""
    head = ATTRIBUTES.sub(" ", without_templates(head))
    if re.search(r"\bnamespace\b", head) or re.search(r"\bextern\s*$", head):
        return "namespace", None
    if re.search(r"\benum\b", head):
        return "other", None
    match = re.search(r"(?:^|[\s;{}])(?:struct|class|union)\b(.*)$", head, re.S)
    if match and "(" not in match.group(1).split(":")[0]:
        return "struct", struct_name(match.group(1))
    return "other", None


def event_types(cleaned: dict) -> set:
    """FunctionSubscribe, every class derived from an event type and every alias of one."""
    types = {BASE_TYPE}
    while True:
        names = "|".join(sorted(types))
        found = set()
        for text in cleaned.values():
            for match in re.finditer(r"\b(?:struct|class)\s+([^{;:]*?)\s*:(?!:)([^{;]*)\{", text):
                if re.search(r"(?:^|[\s,])(?:public|protected|private)?\s*(?:\w+::)*(?:" + names + r")\b(?!\s*::)",
                             match.group(2)):
                    found.add(struct_name(match.group(1)))
            for match in re.finditer(r"\busing\s+(\w+)\s*=\s*(?:\w+::)*(?:" + names + r")\b", text):
                found.add(match.group(1))
        if found <= types:
            return types
        types |= found


def statement_bounds(text: str, pos: int):
    """(start, end, ends_in_body) of the statement around pos: from the last ; { } before it to
    its ; (a {...} initialiser skipped), or to a function body's {."""
    start = max(text.rfind(c, 0, pos) for c in ";{}") + 1
    depth = 0
    i = pos
    while i < len(text):
        c = text[i]
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == ";" and depth <= 0:
            return start, i, False
        elif c == "{" and depth <= 0:
            before = text[start:i].rstrip()
            if before.endswith(")") or re.search(r"\b(?:const|override|final|noexcept)$", before) or \
                    re.match(r"\s*(?:struct|class|union|enum|namespace)\b", text[start:i]):
                return start, i, True
            i = matching(text, i, "{", "}")
            continue
        elif c == "}" and depth <= 0:
            return start, i, False
        i += 1
    return start, len(text), False


def read_declaration(statement: str, types_pattern):
    """What a statement declares: None (not a variable: a function, alias, reference...),
    "unreadable", or (names, static, container)."""
    statement = re.sub(r"^\s*(?:(?:public|protected|private)\s*:)+", "", statement)
    if re.match(r"\s*(?:using|typedef|friend|static_assert|template|struct|class|union|enum|namespace|return|~)\b", statement) \
            or re.match(r"\s*~", statement):
        return None
    static = False
    while True:
        match = LEADING.match(statement)
        if not match or not match.group(0).strip():
            break
        static = static or match.group(1) == "static"
        statement = statement[match.end():]
    head = QUALIFIED_NAME.match(statement)
    if not head:
        return "unreadable"
    type_name = head.group(1)
    rest = statement[head.end():]
    arguments = ""
    if rest.lstrip().startswith("<"):
        open_angle = head.end() + len(rest) - len(rest.lstrip())
        close = matching(statement, open_angle, "<", ">")
        arguments = statement[open_angle:close]
        rest = statement[close:]
    direct = types_pattern.fullmatch(type_name) is not None
    container = not direct and types_pattern.search(arguments) is not None
    if not (direct or container):
        # the event type is in an initialiser, a parameter or a function's return: not a member
        return None
    rest = re.sub(r"^\s*(?:const\b|volatile\b|\s)*", "", rest)
    if re.match(r"[&*]", rest) or re.match(r"\s*::", rest) or re.match(r"\s*\(", rest):
        return None   # a reference, a pointer, a nested name, a constructor or cast
    names = []
    part, nesting = "", 0
    for ch in rest + ",":
        if ch in "({[<":
            nesting += 1
        elif ch in ")}]>":
            nesting -= 1
        if ch == "," and nesting == 0:
            declarator = re.match(r"\s*([&*]*)\s*(\w+)\s*(::)?\s*(\(|\[)?", part)
            if not declarator:
                return "unreadable"
            if declarator.group(1) or declarator.group(3):
                return None   # a reference or pointer, or a static's definition (Class::name)
            if declarator.group(4) == "(":
                return None   # a function returning the event
            names.append(declarator.group(2))
            container = container or declarator.group(4) == "["
            part = ""
        else:
            part += ch
    return names, static, container


def scan(clean: str, types: set):
    """The file's holders {name: {"members", "statics", "containers", "visitor", "line"}} and its
    problems [(line, message)], from its text with comments and literals blanked (strip_code)."""
    problems = []
    names = "|".join(sorted(types))
    types_pattern = re.compile(r"(?:" + names + r")")
    token = re.compile(r"(?<![\w~])(?:" + names + r")\b")
    for directive in PREPROCESSOR.finditer(clean):
        if token.search(directive.group(0)):
            problems.append((clean.count("\n", 0, directive.start()) + 1,
                             "an event type in a macro: the lint cannot read it (declare the event outside the macro)"))
    clean = PREPROCESSOR.sub(blank, clean)

    def line_of(offset):
        return clean.count("\n", 0, offset) + 1

    holders = {}
    stack = []          # (kind, name, open offset)
    last = 0
    seen_statements = set()
    for match in re.finditer(r"[{};]|(?<![\w~])(?:" + names + r")\b", clean):
        c = match.group(0)
        if c == "{":
            kind, name = scope_kind(clean[last:match.start()])
            stack.append((kind, name, match.start()))
            last = match.end()
        elif c == "}":
            if stack:
                kind, name, opened = stack.pop()
                if kind == "struct":
                    holder = holders.setdefault(name, {"members": [], "statics": [], "containers": [], "visitor": None,
                                                       "line": line_of(opened)})
                    body = clean[opened:match.end()]
                    visitor = re.search(r"\bforEachEvent\s*\(\s*\w+\s*&&\s*(\w+)\s*\)\s*(?:const\s*)?\{", body)
                    if visitor:
                        holder["visitor"] = (visitor.group(1),
                                             body[visitor.end() - 1:matching(body, visitor.end() - 1, "{", "}")])
            last = match.end()
        elif c == ";":
            last = match.end()
        else:
            kind, name, _ = stack[-1] if stack else ("namespace", None, 0)
            if kind == "other":
                continue   # in a function body or an initialiser
            start, end, _ = statement_bounds(clean, match.start())
            if start in seen_statements:
                continue
            seen_statements.add(start)
            declared = read_declaration(clean[start:end], types_pattern)
            line = line_of(match.start())
            if declared is None:
                continue
            if declared == "unreadable":
                problems.append((line, "an event type the lint cannot read: declare it as `Type name;`"))
                continue
            members, static, container = declared
            if kind == "namespace":
                for member in members:
                    problems.append((line, f"{member}: an event at namespace scope: no clear at Stop walks it "
                                           "(make it a static member of a class and name it in Engine's clears)"))
                continue
            holder = holders.setdefault(name, {"members": [], "statics": [], "containers": [], "visitor": None,
                                               "line": line})
            for member in members:
                holder["statics" if static else "members"].append((member, line))
                if container:
                    holder["containers"].append(member)
    return holders, problems


def function_body(text: str, name: str) -> str:
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;{]*\)[^;{]*\{", text)
    if not match:
        return ""
    return text[match.end() - 1:matching(text, match.end() - 1, "{", "}")]


def read(path: str) -> str:
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def sources(root: str) -> list[str]:
    found = []
    for directory, subdirs, files in os.walk(os.path.join(root, "engine")):
        rel_dir = os.path.relpath(directory, root).replace(os.sep, "/")
        subdirs[:] = sorted(d for d in subdirs if f"{rel_dir}/{d}" not in SKIPPED_DIRS)
        found.extend(f"{rel_dir}/{f}" for f in sorted(files) if f.endswith(SOURCE_EXTENSIONS))
    return found


def check(root: str, walks: dict = WALKS, transient: dict = TRANSIENT) -> list[str]:
    problems = []
    events_header = os.path.join(root, "engine/core/component/ComponentEvents.h")
    listed = read(events_header) if os.path.exists(events_header) else ""
    start = listed.find("using ComponentsWithEvents")
    type_list = listed[start:listed.find(">;", start)] if start >= 0 else ""
    listed_types = set(re.findall(r"\b(\w+Component)\b", type_list))

    engine_cpp = os.path.join(root, "engine/core/Engine.cpp")
    engine_source = strip_code(read(engine_cpp)) if os.path.exists(engine_cpp) else ""
    static_bodies = {function: function_body(engine_source, function) for function in STATIC_CLEARS}
    texts = {rel: strip_code(read(os.path.join(root, rel))) for rel in sources(root)}
    types = event_types({rel: PREPROCESSOR.sub(blank, text) for rel, text in texts.items()})
    walked = set()
    for rel, text in texts.items():
        holders, file_problems = scan(text, types)
        problems.extend(f"{rel}:{line}: {message}" for line, message in file_problems)
        for name, info in holders.items():
            for member, line in info["statics"]:
                # Engine's own by name (not another class's of the same name), the others qualified
                pattern = (r"(?<![\w:.>])" if name == "Engine" else r"\b" + re.escape(name) + r"::") + \
                    re.escape(member) + r"\b"
                for function, body in static_bodies.items():
                    if not re.search(pattern, body):
                        problems.append(f"{rel}:{line}: {name}.{member}: not in {function}")
            if not info["members"] or rel in transient:
                continue
            parameter, visitor = info["visitor"] or ("f", "")
            for member, line in info["members"]:
                listed_here = (r"\b" if member in info["containers"] else r"\b" + parameter + r"\(\s*") + \
                    re.escape(member) + r"\b"
                if not re.search(listed_here, visitor):
                    problems.append(f"{rel}:{line}: {name}.{member}: not in {name}::forEachEvent")
            if rel.startswith(COMPONENT_DIR):
                if name not in listed_types:
                    problems.append(f"{rel}:{info['line']}: {name}: has events but is not in ComponentsWithEvents (ComponentEvents.h)")
            elif name not in walks:
                problems.append(f"{rel}:{info['line']}: {name}: has events but no clear at Stop walks them "
                                f"(call its forEachEvent from one and add it to WALKS in tools/check_event_clears.py)")
            else:
                walked.add(name)
    for name in sorted(walked):
        for path, function in walks[name]:
            full = os.path.join(root, path)
            body = function_body(strip_code(read(full)), function) if os.path.exists(full) else ""
            if not re.search(r"\bforEachEvent\s*\(", body):
                problems.append(f"{path}: {function}: does not call {name}'s forEachEvent")
    return problems


def self_test() -> int:
    def write(root, rel, text, mode="w"):
        os.makedirs(os.path.dirname(os.path.join(root, rel)), exist_ok=True)
        with open(os.path.join(root, rel), mode) as f:
            f.write(text)

    walks = {"PhysicsSystem": [("engine/core/subsystem/PhysicsSystem.cpp", "PhysicsSystem::clearSubscriptions")],
             "Action": [("engine/core/input/Actions.cpp", "Actions::unload")]}
    transient = {"engine/core/particles/Legacy.h": "a view"}
    with tempfile.TemporaryDirectory() as root:
        write(root, "engine/core/util/FunctionSubscribe.h",
              "namespace jasperengine {\n    template<typename T> class FunctionSubscribe;\n"
              "    template<typename Ret, typename ...Args>\n    class FunctionSubscribe<Ret(Args...)> {\n"
              "        FunctionSubscribe(const FunctionSubscribe& other) {}\n"
              "        FunctionSubscribe& operator = (const FunctionSubscribe& other) { return *this; }\n"
              "        ~FunctionSubscribe() {}\n    };\n}\n")
        # a class derived from FunctionSubscribe is an event type too (an Ease)
        write(root, "engine/core/action/Ease.h",
              "namespace jasperengine {\n    class Ease: public FunctionSubscribe<float(float)> {\n"
              "        using EaseFunction = FunctionSubscribe<float(float)>;\n        using EaseFunction::clear;\n"
              "        Ease(): EaseFunction(std::function<float(float)>(Ease::linear)){}\n"
              "        explicit Ease(int type): Ease(){}\n        Ease& operator = (const Ease& ease){ return *this; }\n"
              "        bool operator == (const Ease& ease) const{ return true; }\n    };\n}\n")
        write(root, "engine/core/component/ComponentEvents.h",
              "using ComponentsWithEvents = ComponentTypeList<\n    GoodComponent, TweenComponent>;\n")
        write(root, "engine/core/component/Good.h",
              "struct GoodComponent{\n    FunctionSubscribe<void()> onA;\n    jasperengine::FunctionSubscribe<void(int,\n"
              "        std::string)> onB;\n    std::array<FunctionSubscribe<void()>, 4> onSlots;\n"
              "    FunctionSubscribe<void()> onMore[2];\n"
              "    template<typename F> void forEachEvent(F&& f) {\n        f(onA); f(onB);\n"
              "        for (auto& e : onSlots) f(e);\n        for (auto& e : onMore) f(e);\n    }\n};\n")
        write(root, "engine/core/component/Tween.h",
              "struct JASPERENGINE_API TweenComponent{\n    float time = 0;\n    Ease function;\n"
              "    template<typename F> void forEachEvent(F&& f) { f(function); }\n};\n")
        write(root, "engine/core/subsystem/PhysicsSystem.h",
              "class JASPERENGINE_API PhysicsSystem : public SubSystem {\n    public:\n"
              "        template<typename F> void forEachEvent(F&& f) {\n            f(onHit);\n        }\n"
              "        FunctionSubscribe<void(Entity)> onHit;\n"
              # a getter, a reference, a parameter and a local in an inline function are not events
              "        FunctionSubscribe<void(int)>& onRestore();\n        FunctionSubscribe<void()>& ref;\n"
              "        void fire(FunctionSubscribe<void()>& event, bool a) {\n"
              "            FunctionSubscribe<void(Entity)> copy = onHit;\n"
              "            const char* text = \"{\";\n        }\n};\n")
        write(root, "engine/core/subsystem/PhysicsSystem.cpp",
              "void PhysicsSystem::clearSubscriptions(){\n    forEachEvent([](auto& e) { e.clear(); });\n}\n")
        write(root, "engine/core/input/Actions.h",
              "class Action {\n    public:\n        FunctionSubscribe<void(Event&)> onStarted, onCanceled;\n"
              "        template<typename F> void forEachEvent(F&& f) { f(onStarted); f(onCanceled); }\n};\n")
        write(root, "engine/core/input/Actions.cpp",
              "void Actions::unload(){\n    for (auto& a : actions) a.forEachEvent([](auto& e) { e.clear(); });\n}\n")
        write(root, "engine/core/particles/Legacy.h", "struct Modifier{\n    Ease function;\n};\n")
        write(root, "engine/core/Engine.h",
              "class Engine{\n    static FunctionSubscribe<void()> onUpdate;\n    static FunctionSubscribe<void()> onLowMemory;\n"
              "    static FunctionSubscribe<void()>& getOnInit();\n};\n")
        write(root, "engine/core/device/Device.h",
              "class Device{\n    JASPERENGINE_API static FunctionSubscribe<void()> onLowMemory;\n};\n")
        # a static's definition and a getter's at namespace scope are not events
        write(root, "engine/core/device/Device.cpp",
              "FunctionSubscribe<void()> Device::onLowMemory;\n"
              "FunctionSubscribe<void()>& Engine::getOnInit() {\n    static FunctionSubscribe<void()> event;\n    return event;\n}\n")
        write(root, "engine/core/Engine.cpp",
              "void Engine::clearAllSubscriptions(bool all) {\n    onUpdate.clear();\n    onLowMemory.clear();\n"
              "    Device::onLowMemory.clear();\n}\n"
              "void Engine::removeSubscriptionsByTag(const std::string& s) {\n    onUpdate.removeByTagSubstring(s);\n"
              "    onLowMemory.removeByTagSubstring(s);\n    Device::onLowMemory.removeByTagSubstring(s);\n}\n")
        # vendored code is not the engine's
        write(root, "engine/libs/thing/Thing.h", "struct Thing{\n    FunctionSubscribe<void()> onIgnored;\n};\n")
        if check(root, walks, transient):
            print(f"self-test: a clean tree has problems: {check(root, walks, transient)}", file=sys.stderr)
            return 1
        # a member left out of its visitor, a component left out of the list, a physics event, an
        # Engine event left out of a clear, a static of another class left out of both, an
        # Engine event only another class's same-named one stands for, a holder outside the
        # components no clear walks, a walked holder's member, a walk that lost its visitor call,
        # an Ease a tween holds outside its visitor, a qualified type split across lines, a
        # container, an exported static, an alignas holder, an event at namespace scope and one in
        # a macro
        write(root, "engine/core/component/Bad.h",
              "struct BadComponent{\n    FunctionSubscribe<void()> onX;\n    FunctionSubscribe<void(std::map<int, int>)> onY, onZ = {};\n"
              "    template<typename F> void forEachEvent(F&& f) { f(onX); f(onZ); }\n};\n")
        write(root, "engine/core/subsystem/PhysicsSystem.h", "struct Extra{\n    FunctionSubscribe<void()> onLost;\n};\n", "a")
        write(root, "engine/core/Engine.h", "class Engine2{\n    static FunctionSubscribe<void()> onForgotten;\n};\n", "a")
        write(root, "engine/core/audio/Music.h",
              "namespace je {\n    class JASPERENGINE_API Music {\n        public:\n"
              "        JASPERENGINE_API static inline jasperengine::FunctionSubscribe<void(int,\n"
              "            int)> onBeat;\n    };\n}\n")
        write(root, "engine/core/Engine.cpp",
              read(os.path.join(root, "engine/core/Engine.cpp")).replace("    onLowMemory.clear();\n", ""))
        write(root, "engine/core/subsystem/UISystem.h",
              "class UISystem : public SubSystem {\n    FunctionSubscribe<void(int)> onFocusMoved;\n};\n")
        write(root, "engine/core/input/Actions.h",
              read(os.path.join(root, "engine/core/input/Actions.h")).replace("onCanceled;", "onCanceled;\n"
                                                                             "        FunctionSubscribe<void(Event&)> onPerformed;"))
        write(root, "engine/core/input/Actions.cpp", "void Actions::unload(){\n    actions.clear();\n}\n")
        write(root, "engine/core/component/Tween.h",
              read(os.path.join(root, "engine/core/component/Tween.h")).replace("f(function);", ""))
        write(root, "engine/core/component/Good.h",
              read(os.path.join(root, "engine/core/component/Good.h")).replace("for (auto& e : onSlots) f(e);", ""))
        write(root, "engine/core/subsystem/Aligned.h",
              "struct alignas(16) [[nodiscard]] AlignedHolder {\n    jasperengine::FunctionSubscribe<\n        void()> onTick;\n};\n")
        write(root, "engine/core/subsystem/Globals.cpp",
              "namespace jasperengine {\n    FunctionSubscribe<void()> gOnSomething;\n    namespace {\n"
              "        static FunctionSubscribe<void(int)> sOnOther;\n    }\n}\n"
              "#define DECLARE_EVENT(name) FunctionSubscribe<void()> name;\n")
        found = check(root, walks, transient)
        expected = ["Bad.h:3: BadComponent.onY", "BadComponent: has events", "Extra.onLost", "Extra: has events but no clear",
                    "Engine2.onForgotten: not in Engine::clearAllSubscriptions",
                    "Engine2.onForgotten: not in Engine::removeSubscriptionsByTag",
                    "Music.h:4: Music.onBeat: not in Engine::clearAllSubscriptions",
                    "Music.onBeat: not in Engine::removeSubscriptionsByTag",
                    "Engine.onLowMemory: not in Engine::clearAllSubscriptions",
                    "UISystem.onFocusMoved: not in UISystem::forEachEvent", "UISystem: has events but no clear",
                    "Action.onPerformed: not in Action::forEachEvent",
                    "Actions::unload: does not call Action's forEachEvent",
                    "TweenComponent.function: not in TweenComponent::forEachEvent",
                    "GoodComponent.onSlots: not in GoodComponent::forEachEvent",
                    "Aligned.h:2: AlignedHolder.onTick: not in AlignedHolder::forEachEvent",
                    "AlignedHolder: has events but no clear",
                    "Globals.cpp:2: gOnSomething: an event at namespace scope",
                    "Globals.cpp:4: sOnOther: an event at namespace scope",
                    "Globals.cpp:7: an event type in a macro"]
        missing = [e for e in expected if not any(e in problem for problem in found)]
        if missing or len(found) != len(expected):
            print(f"self-test: expected {expected},\ngot {found},\nmissing {missing}", file=sys.stderr)
            return 1
    print("check_event_clears self-test: ok")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    problems = check(os.path.abspath(args.root))
    for problem in problems:
        print(problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
