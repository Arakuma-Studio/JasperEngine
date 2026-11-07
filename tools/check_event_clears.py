#!/usr/bin/env python3
"""Fails when an engine event could outlive Stop: a FunctionSubscribe member left out of its clear.

At Stop the editor unloads the C++ script library, so every callback a script subscribed must be
gone first (Engine::clearAllSubscriptions, Engine::clearComponentSubscriptions, the holders' own
clears), and a hot reload removes them by tag (Scene::removeSubscriptionsByTag,
Engine::removeSubscriptionsByTag). The clears walk lists, and this checks every FunctionSubscribe
member of every struct or class in the engine's sources (engine/, not its libraries or tests):

- a holder's instance events are each listed as f(member) in its forEachEvent;
- a component (engine/core/component/) with events is in ComponentsWithEvents (ComponentEvents.h);
- any other holder is in WALKS below, whose clears at Stop (and on tag removal) call its
  forEachEvent: a new holder gets its clears and an entry there;
- a static event is named in Engine::clearAllSubscriptions and Engine::removeSubscriptionsByTag
  (Engine.cpp): Engine's own by name, any other class's as Class::member.

    python3 tools/check_event_clears.py [--root DIR] [--self-test]

Prints `file: struct.member: <what>` for each miss and exits 1 when there is one.

A declaration may name several events (`FunctionSubscribe<void()> onA, onB;`) and give them
initialisers (`= ...`, `{...}`); each one counts. Locals, parameters, references and getters
(`FunctionSubscribe<...>& onX()`) are not members. Types derived from FunctionSubscribe (Ease, a
tween's function) are values, not events, and are not checked.
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
}
STATIC_CLEARS = ("Engine::clearAllSubscriptions", "Engine::removeSubscriptionsByTag")
SOURCE_EXTENSIONS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".mm", ".m")
# not engine code: vendored libraries, tests, the IDE projects
SKIPPED_DIRS = ("engine/libs", "engine/tests", "engine/workspaces")
COMPONENT_DIR = "engine/core/component/"

DECLARATION = re.compile(r"^\s*((?:(?:static|inline|mutable)\s+)*)FunctionSubscribe<")
OPEN = re.compile(r"^\s*(?:template\s*<[^>]*>\s*)?(?:struct|class)\s+(?:JASPERENGINE_API\s+)?(\w+)\b[^;]*$")
VISITOR = "forEachEvent(F&& f)"


def event_names(code: str) -> tuple[list[str], bool]:
    """The names a FunctionSubscribe member declaration on this line declares, and whether it is
    static: `FunctionSubscribe<void(A<B>, C)> onX, onY = ..., onZ{...};` gives onX, onY, onZ."""
    match = DECLARATION.match(code)
    if not match:
        return [], False
    i = code.index("FunctionSubscribe<") + len("FunctionSubscribe<")
    depth = 1
    while i < len(code) and depth:
        depth += {"<": 1, ">": -1}.get(code[i], 0)
        i += 1
    rest = code[i:].split(";")[0]
    names, part, nesting = [], "", 0
    for ch in rest + ",":
        if ch in "({<[":
            nesting += 1
        elif ch in ")}>]":
            nesting -= 1
        if ch == "," and nesting == 0:
            # a reference, pointer or function (a getter) is not a member event
            name = re.match(r"\s*(\w+)\s*(?:$|=|\{)", part)
            if name:
                names.append(name.group(1))
            part = ""
        else:
            part += ch
    return names, "static" in match.group(1)


# comments, raw strings, strings and character literals (not a digit separator: 1'000)
LITERAL = re.compile(r"//[^\n]*|/\*.*?\*/|\bR\"([^(\s]*)\(.*?\)\1\"|\"(?:\\.|[^\"\\\n])*\"|"
                     r"(?:(?<!\w)|\b(?:u8|[LuU]))'(?:\\[^'\n]*|[^'\\\n])'", re.S)


def strip_code(text: str) -> str:
    """Comments, string and character literals blanked (newlines kept), so braces in them do not
    count."""
    return LITERAL.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def structs_of(text: str) -> dict[str, dict]:
    """struct name -> {"members": [...], "statics": [...], "visitor": text of its forEachEvent body
    or None}; only declarations directly in a struct's body count (not in its functions)."""
    found: dict[str, dict] = {}
    stack: list = []          # one entry per open brace: the struct's name, or None (any other)
    pending = None
    visitor_owner = None
    visitor_depth = 0
    visitor_text: list[str] = []

    def entry(name):
        return found.setdefault(name, {"members": [], "statics": [], "visitor": None})

    for code in strip_code(text).split("\n"):
        match = OPEN.match(code)
        if match and "enum" not in code:
            pending = match.group(1)
        if VISITOR in code and stack and stack[-1] is not None:
            visitor_owner = stack[-1]
            visitor_depth = len(stack)
            visitor_text = []
        if visitor_owner is not None:
            visitor_text.append(code)
        names, static = event_names(code)
        if names and stack and stack[-1] is not None:
            entry(stack[-1])["statics" if static else "members"].extend(names)
        for ch in code:
            if ch == "{":
                stack.append(pending)
                pending = None
            elif ch == "}":
                if stack:
                    stack.pop()
                if visitor_owner is not None and len(stack) == visitor_depth:
                    entry(visitor_owner)["visitor"] = "\n".join(visitor_text)
                    visitor_owner = None
            elif ch == ";":
                pending = None
    return found


def function_body(text: str, name: str) -> str:
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;{]*\)[^;{]*\{", text)
    if not match:
        return ""
    open_brace = match.end() - 1
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace:i + 1]
    return text[open_brace:]


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


def check(root: str, walks: dict = WALKS) -> list[str]:
    problems = []
    events_header = os.path.join(root, "engine/core/component/ComponentEvents.h")
    listed = read(events_header) if os.path.exists(events_header) else ""
    start = listed.find("using ComponentsWithEvents")
    type_list = listed[start:listed.find(">;", start)] if start >= 0 else ""
    listed_types = set(re.findall(r"\b(\w+Component)\b", type_list))

    engine_cpp = os.path.join(root, "engine/core/Engine.cpp")
    engine_source = strip_code(read(engine_cpp)) if os.path.exists(engine_cpp) else ""
    static_bodies = {function: function_body(engine_source, function) for function in STATIC_CLEARS}
    walked = set()
    for rel in sources(root):
        for name, info in structs_of(read(os.path.join(root, rel))).items():
            for member in info["statics"]:
                # Engine's own by name (not another class's of the same name), the others qualified
                pattern = (r"(?<![\w:.>])" if name == "Engine" else r"\b" + re.escape(name) + r"::") + \
                    re.escape(member) + r"\b"
                for function, body in static_bodies.items():
                    if not re.search(pattern, body):
                        problems.append(f"{rel}: {name}.{member}: not in {function}")
            if not info["members"]:
                continue
            visitor = info["visitor"] or ""
            for member in info["members"]:
                if not re.search(r"\bf\(\s*" + re.escape(member) + r"\s*\)", visitor):
                    problems.append(f"{rel}: {name}.{member}: not in {name}::forEachEvent")
            if rel.startswith(COMPONENT_DIR):
                if name not in listed_types:
                    problems.append(f"{rel}: {name}: has events but is not in ComponentsWithEvents (ComponentEvents.h)")
            elif name not in walks:
                problems.append(f"{rel}: {name}: has events but no clear at Stop walks them "
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
    with tempfile.TemporaryDirectory() as root:
        write(root, "engine/core/component/ComponentEvents.h",
              "using ComponentsWithEvents = ComponentTypeList<\n    GoodComponent>;\n")
        write(root, "engine/core/component/Good.h",
              "struct GoodComponent{\n    FunctionSubscribe<void()> onA;\n    FunctionSubscribe<void(int)> onB;\n"
              "    template<typename F> void forEachEvent(F&& f) { f(onA); f(onB); }\n};\n")
        write(root, "engine/core/subsystem/PhysicsSystem.h",
              "class JASPERENGINE_API PhysicsSystem : public SubSystem {\n    public:\n"
              "        template<typename F> void forEachEvent(F&& f) {\n            f(onHit);\n        }\n"
              "        FunctionSubscribe<void(Entity)> onHit;\n"
              # a getter, a reference and a local in an inline function are not members
              "        FunctionSubscribe<void(int)>& onRestore();\n"
              "        void fire(bool a) {\n            FunctionSubscribe<void(Entity)> copy = onHit;\n"
              "            const char* text = \"{\";\n        }\n};\n")
        write(root, "engine/core/subsystem/PhysicsSystem.cpp",
              "void PhysicsSystem::clearSubscriptions(){\n    forEachEvent([](auto& e) { e.clear(); });\n}\n")
        write(root, "engine/core/input/Actions.h",
              "class Action {\n    public:\n        FunctionSubscribe<void(Event&)> onStarted, onCanceled;\n"
              "        template<typename F> void forEachEvent(F&& f) { f(onStarted); f(onCanceled); }\n};\n")
        write(root, "engine/core/input/Actions.cpp",
              "void Actions::unload(){\n    for (auto& a : actions) a.forEachEvent([](auto& e) { e.clear(); });\n}\n")
        write(root, "engine/core/Engine.h",
              "class Engine{\n    static FunctionSubscribe<void()> onUpdate;\n    static FunctionSubscribe<void()> onLowMemory;\n};\n")
        write(root, "engine/core/device/Device.h", "class Device{\n    static FunctionSubscribe<void()> onLowMemory;\n};\n")
        # the class's definitions of its statics in a source are not declarations
        write(root, "engine/core/device/Device.cpp", "FunctionSubscribe<void()> Device::onLowMemory;\n")
        write(root, "engine/core/Engine.cpp",
              "void Engine::clearAllSubscriptions(bool all) {\n    onUpdate.clear();\n    onLowMemory.clear();\n"
              "    Device::onLowMemory.clear();\n}\n"
              "void Engine::removeSubscriptionsByTag(const std::string& s) {\n    onUpdate.removeByTagSubstring(s);\n"
              "    onLowMemory.removeByTagSubstring(s);\n    Device::onLowMemory.removeByTagSubstring(s);\n}\n")
        # vendored code is not the engine's
        write(root, "engine/libs/thing/Thing.h", "struct Thing{\n    FunctionSubscribe<void()> onIgnored;\n};\n")
        if check(root, walks):
            print(f"self-test: a clean tree has problems: {check(root, walks)}", file=sys.stderr)
            return 1
        # a member left out of its visitor, a component left out of the list, a physics event, an
        # Engine event left out of a clear, a static of another class left out of both, an
        # Engine event only another class's same-named one stands for, a holder outside the
        # components no clear walks, a walked holder's member and a walk that lost its visitor call
        write(root, "engine/core/component/Bad.h",
              "struct BadComponent{\n    FunctionSubscribe<void()> onX;\n    FunctionSubscribe<void(std::map<int, int>)> onY, onZ = {};\n"
              "    template<typename F> void forEachEvent(F&& f) { f(onX); f(onZ); }\n};\n")
        write(root, "engine/core/subsystem/PhysicsSystem.h", "struct Extra{\n    FunctionSubscribe<void()> onLost;\n};\n", "a")
        write(root, "engine/core/Engine.h", "class Engine2{\n    static FunctionSubscribe<void()> onForgotten;\n};\n", "a")
        write(root, "engine/core/audio/Music.h",
              "namespace je {\n    class JASPERENGINE_API Music {\n        public:\n"
              "        static inline FunctionSubscribe<void(int, int)> onBeat;\n    };\n}\n")
        write(root, "engine/core/Engine.cpp",
              read(os.path.join(root, "engine/core/Engine.cpp")).replace("    onLowMemory.clear();\n", ""))
        write(root, "engine/core/subsystem/UISystem.h",
              "class UISystem : public SubSystem {\n    FunctionSubscribe<void(int)> onFocusMoved;\n};\n")
        write(root, "engine/core/input/Actions.h",
              read(os.path.join(root, "engine/core/input/Actions.h")).replace("onCanceled;", "onCanceled;\n"
                                                                             "        FunctionSubscribe<void(Event&)> onPerformed;"))
        write(root, "engine/core/input/Actions.cpp", "void Actions::unload(){\n    actions.clear();\n}\n")
        found = check(root, walks)
        expected = ["BadComponent.onY", "BadComponent: has events", "Extra.onLost", "Extra: has events but no clear",
                    "Engine2.onForgotten: not in Engine::clearAllSubscriptions",
                    "Engine2.onForgotten: not in Engine::removeSubscriptionsByTag",
                    "Music.onBeat: not in Engine::clearAllSubscriptions",
                    "Music.onBeat: not in Engine::removeSubscriptionsByTag",
                    "Engine.onLowMemory: not in Engine::clearAllSubscriptions",
                    "UISystem.onFocusMoved: not in UISystem::forEachEvent", "UISystem: has events but no clear",
                    "Action.onPerformed: not in Action::forEachEvent",
                    "Actions::unload: does not call Action's forEachEvent"]
        missing = [e for e in expected if not any(e in problem for problem in found)]
        if missing or len(found) != len(expected):
            print(f"self-test: expected {expected}, got {found}", file=sys.stderr)
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
