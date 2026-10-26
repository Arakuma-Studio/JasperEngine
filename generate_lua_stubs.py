#!/usr/bin/env python3
"""Deprecated (kept for one release): the Lua API is described once by tools/lua_api (S23 R29).
  generate_lua_stubs.py <binding_dir> <engine_core_dir> <output_header> [--out-dir DIR]
is `python3 -m lua_api all --binding-dir ... --core-dir ... --json ... --stubs-header ... --stubs-dir ...`."""
import argparse, os, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tools'))
from lua_api import extract, stubs  # noqa: E402
ap = argparse.ArgumentParser(description=__doc__); ap.add_argument('binding_dir'); ap.add_argument('core_dir'); ap.add_argument('output_header'); ap.add_argument('--out-dir')
a = ap.parse_args()
stubs.write_stubs(extract.extract(a.binding_dir, a.core_dir), a.output_header, a.out_dir)
