#!/usr/bin/env python3
# (c) JasperDev and contributors
# SPDX-License-Identifier: MIT
"""The web crash hooks (engine/platform/emscripten/crash_pre.js) read the Gameplay Journal's ring
from the heap at any address: a wasm heap past 2 GiB (memory64-free builds reach 4 GiB) puts its
counters at pointers >= 2^31, which a signed shift turns into negative indices.

Runs the hook under node with a sparse heap (a Proxy, so no 2 GiB array is made)."""

import json
import os
import shutil
import subprocess
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HOOK = os.path.join(ROOT, "engine", "platform", "emscripten", "crash_pre.js")

# the ring holds two whole entries; cursor and written are u64 counters in the heap
SCRIPT = r"""
const fs = require("fs");
const cells = new Map();
const heap = (bytes) => new Proxy({}, {
  get: (_, key) => { const i = Number(key); return cells.get(bytes + ":" + i) || 0; },
  set: (_, key, value) => { cells.set(bytes + ":" + Number(key), value); return true; },
});
globalThis.HEAPU8 = heap(1);
globalThis.HEAPU32 = heap(4);
const errors = [];
console.error = (...args) => errors.push(args.join(" "));
globalThis.Module = {};
eval(fs.readFileSync(process.argv[1], "utf8"));

const base = Number(process.argv[2]);
const ring = base + 0x1000, cursor = base + 0x10, written = base + 0x18;
const text = "+1.00 first\n+2.00 second\n";
for (let i = 0; i < text.length; i++) HEAPU8[ring + i] = text.charCodeAt(i);
HEAPU32[cursor / 4] = text.length;
HEAPU32[written / 4] = 2;
Module.jasperengineCrash.configure({ journal: { ring, capacity: 4096, cursor, written, header: "journal\n" } });
Module.jasperengineCrash.report("test");
process.stdout.write(JSON.stringify(errors.join("\n")));
"""


def run_hook(base):
    node = shutil.which("node") or shutil.which("nodejs")
    if not node:
        raise AssertionError("node is not on PATH: the lint lane's runner has it (install nodejs locally)")
    out = subprocess.run([node, "-e", SCRIPT, HOOK, str(base)], capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


class CrashPreJournal(unittest.TestCase):
    def test_reads_the_journal_below_2_gib(self):
        report = run_hook(0x100000)
        self.assertIn("--- journal ---", report)
        self.assertIn("+2.00 second", report)

    def test_reads_the_journal_past_2_gib(self):
        report = run_hook(0x90000000)
        self.assertIn("--- journal ---", report, "counters at a pointer >= 2^31 read as index < 0")
        self.assertIn("+1.00 first", report)
        self.assertIn("dropped: 0", report)


if __name__ == "__main__":
    unittest.main()
