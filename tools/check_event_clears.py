#!/usr/bin/env python3
"""Fails when an engine event could outlive Stop: a FunctionSubscribe member left out of its clear.

At Stop the editor unloads the C++ script library, so every callback a script subscribed must be
gone first (Engine::clearComponentSubscriptions, Engine::clearAllSubscriptions,
PhysicsSystem::clearSubscriptions), and a hot reload removes them by tag
(Scene::removeSubscriptionsByTag, Engine::removeSubscriptionsByTag). The clears walk lists:

- every component struct under engine/core/component/ with FunctionSubscribe members lists each of
  them as f(member) in its forEachEvent, and is in ComponentsWithEvents (ComponentEvents.h);
- PhysicsSystem lists each of its events in its forEachEvent;
- every static FunctionSubscribe of Engine.h is named in Engine::clearAllSubscriptions and
  Engine::removeSubscriptionsByTag (Engine.cpp).

    python3 tools/check_event_clears.py [--root DIR] [--self-test]

Prints `file: struct.member: <what>` for each miss and exits 1 when there is one.

A declaration may name several events (`FunctionSubscribe<void()> onA, onB;`) and give them
initialisers (`= ...`, `{...}`); each one counts. Events elsewhere (`Log::onMessageReceived`, a
subsystem other than PhysicsSystem, an event in a .cpp) are outside what this checks: they are not
script-owned engine events the clears at Stop walk.
"""

import argparse
import glob
import os
import re
import sys
import tempfile

DECLARATION = re.compile(r"^\s*(static\s+)?FunctionSubscribe<")


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
            name = re.match(r"\s*(\w+)", part)
            if name:
                names.append(name.group(1))
            part = ""
        else:
            part += ch
    return names, bool(match.group(1))
OPEN = re.compile(r"^\s*(?:struct|class)\s+(?:JASPERENGINE_API\s+)?(\w+)\b[^;]*$")
VISITOR = "forEachEvent(F&& f)"


def strip_comment(line: str) -> str:
    return line.split("//", 1)[0]


def structs_of(text: str) -> dict[str, dict]:
    """struct name -> {"members": [...], "visitor": text of its forEachEvent body or None}"""
    found: dict[str, dict] = {}
    stack: list[tuple[str, int]] = []
    depth = 0
    pending = None
    visitor_owner = None
    visitor_depth = 0
    visitor_text: list[str] = []
    for line in text.split("\n"):
        code = strip_comment(line)
        match = OPEN.match(code)
        if match and "enum" not in code:
            pending = match.group(1)
        if VISITOR in code and stack:
            visitor_owner = stack[-1][0]
            visitor_depth = depth
            visitor_text = []
        if visitor_owner is not None:
            visitor_text.append(code)
        names, static = event_names(code)
        if names and stack and not static:
            found.setdefault(stack[-1][0], {"members": [], "visitor": None})["members"].extend(names)
        for ch in code:
            if ch == "{":
                depth += 1
                if pending:
                    stack.append((pending, depth))
                    pending = None
            elif ch == "}":
                if stack and stack[-1][1] == depth:
                    stack.pop()
                depth -= 1
                if visitor_owner is not None and depth == visitor_depth:
                    found.setdefault(visitor_owner, {"members": [], "visitor": None})["visitor"] = "\n".join(visitor_text)
                    visitor_owner = None
    return found


def function_body(text: str, signature: str) -> str:
    start = text.find(signature)
    if start < 0:
        return ""
    open_brace = text.find("{", start)
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


def check(root: str) -> list[str]:
    problems = []
    events_header = os.path.join(root, "engine/core/component/ComponentEvents.h")
    listed = read(events_header) if os.path.exists(events_header) else ""
    start = listed.find("using ComponentsWithEvents")
    type_list = listed[start:listed.find(">;", start)] if start >= 0 else ""
    listed_types = set(re.findall(r"\b(\w+Component)\b", type_list))

    headers = sorted(glob.glob(os.path.join(root, "engine/core/component/*.h")))
    physics = os.path.join(root, "engine/core/subsystem/PhysicsSystem.h")
    if os.path.exists(physics):
        headers.append(physics)
    for path in headers:
        rel = os.path.relpath(path, root)
        for name, info in structs_of(read(path)).items():
            if not info["members"]:
                continue
            visitor = info["visitor"] or ""
            for member in info["members"]:
                if not re.search(r"\bf\(\s*" + re.escape(member) + r"\s*\)", visitor):
                    problems.append(f"{rel}: {name}.{member}: not in {name}::forEachEvent")
            if path != physics and name not in listed_types:
                problems.append(f"{rel}: {name}: has events but is not in ComponentsWithEvents (ComponentEvents.h)")

    engine_h = os.path.join(root, "engine/core/Engine.h")
    engine_cpp = os.path.join(root, "engine/core/Engine.cpp")
    if os.path.exists(engine_h) and os.path.exists(engine_cpp):
        statics = []
        for line in read(engine_h).split("\n"):
            names, static = event_names(strip_comment(line))
            if static:
                statics.extend(names)
        source = read(engine_cpp)
        for function in ("Engine::clearAllSubscriptions", "Engine::removeSubscriptionsByTag"):
            body = function_body(source, "void " + function + "(")
            for member in statics:
                if not re.search(r"\b" + re.escape(member) + r"\b", body):
                    problems.append(f"engine/core/Engine.h: Engine.{member}: not in {function}")
    return problems


def self_test() -> int:
    with tempfile.TemporaryDirectory() as root:
        os.makedirs(os.path.join(root, "engine/core/component"))
        os.makedirs(os.path.join(root, "engine/core/subsystem"))
        with open(os.path.join(root, "engine/core/component/ComponentEvents.h"), "w") as f:
            f.write("using ComponentsWithEvents = ComponentTypeList<\n    GoodComponent>;\n")
        with open(os.path.join(root, "engine/core/component/Good.h"), "w") as f:
            f.write("struct GoodComponent{\n    FunctionSubscribe<void()> onA;\n    FunctionSubscribe<void(int)> onB;\n"
                    "    template<typename F> void forEachEvent(F&& f) { f(onA); f(onB); }\n};\n")
        with open(os.path.join(root, "engine/core/subsystem/PhysicsSystem.h"), "w") as f:
            f.write("class JASPERENGINE_API PhysicsSystem : public SubSystem {\n    public:\n"
                    "        template<typename F> void forEachEvent(F&& f) {\n            f(onHit);\n        }\n"
                    "        FunctionSubscribe<void(Entity)> onHit;\n};\n")
        with open(os.path.join(root, "engine/core/Engine.h"), "w") as f:
            f.write("class Engine{\n    static FunctionSubscribe<void()> onUpdate;\n};\n")
        with open(os.path.join(root, "engine/core/Engine.cpp"), "w") as f:
            f.write("void Engine::clearAllSubscriptions(bool all) {\n    onUpdate.clear();\n}\n"
                    "void Engine::removeSubscriptionsByTag(const std::string& s) {\n    onUpdate.removeByTagSubstring(s);\n}\n")
        if check(root):
            print(f"self-test: a clean tree has problems: {check(root)}", file=sys.stderr)
            return 1
        # a member left out of its visitor, a component left out of the list, a physics event and an
        # Engine event left out of a clear
        with open(os.path.join(root, "engine/core/component/Bad.h"), "w") as f:
            f.write("struct BadComponent{\n    FunctionSubscribe<void()> onX;\n    FunctionSubscribe<void(std::map<int, int>)> onY, onZ = {};\n"
                    "    template<typename F> void forEachEvent(F&& f) { f(onX); f(onZ); }\n};\n")
        with open(os.path.join(root, "engine/core/subsystem/PhysicsSystem.h"), "a") as f:
            f.write("struct Extra{\n    FunctionSubscribe<void()> onLost;\n};\n")
        with open(os.path.join(root, "engine/core/Engine.h"), "a") as f:
            f.write("class Engine2{\n    static FunctionSubscribe<void()> onForgotten;\n};\n")
        found = check(root)
        expected = ["BadComponent.onY", "BadComponent: has events", "Extra.onLost",
                    "Engine.onForgotten: not in Engine::clearAllSubscriptions",
                    "Engine.onForgotten: not in Engine::removeSubscriptionsByTag"]
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
