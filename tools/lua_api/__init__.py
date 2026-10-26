# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The engine's Lua API, described once (S23 R29): parser.py reads the LuaBridge bindings and the
engine headers, extract.py writes lua-api.json, and three emitters read only that JSON: the LuaLS
stubs (stubs.py), the editor's completion header (suggestions.py) and the Markdown reference
(markdown.py). `python3 -m lua_api --help` (with tools/ on PYTHONPATH) runs them; CMake does at
build time."""
