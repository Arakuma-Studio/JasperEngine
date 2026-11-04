"""Tests of tools/check_fanout.py (python3 -I tools/test_check_fanout.py): the limits' red and green,
what counts as a unit of the editor. No build tree."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_fanout  # noqa: E402

DEPS = """\
CMakeFiles/jasperengine-editor-objects.dir/editor/A.cpp.o: #deps 3, deps mtime 1 (VALID)
    /src/editor/A.cpp
    /src/engine/core/component/Transform.h
    /src/editor/A.h

CMakeFiles/jasperengine-editor-objects.dir/editor/B.cpp.o: #deps 2, deps mtime 1 (STALE)
    /src/editor/B.cpp
    /src/engine/core/component/Transform.h

engine/CMakeFiles/jasperengine.dir/core/C.cpp.o: #deps 2, deps mtime 1 (VALID)
    /src/engine/core/C.cpp
    /src/engine/core/component/Transform.h

engine/libs/x/CMakeFiles/x.dir/y.c.o: #deps 1, deps mtime 1 (VALID)
    /src/engine/libs/x/y.c

"""


class FanOut(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / check_fanout.COMPONENT_DIR).mkdir(parents=True)
        (self.root / check_fanout.COMPONENT_DIR / "Transform.h").write_text("")
        (self.root / check_fanout.COMPONENT_DIR / "Unused.h").write_text("")

    def tearDown(self):
        self.tmp.cleanup()

    def test_units_by_target(self):
        units = check_fanout.parse_deps(DEPS, Path("/src/build/linux/editor"), self.root)
        self.assertEqual(len(units), 4)
        fan = check_fanout.fan_out(units)
        self.assertEqual(fan["engine/core/component/Transform.h"], {"editor": 2, "other": 1})
        self.assertEqual(fan["editor/A.h"], {"editor": 1, "other": 0})
        self.assertNotIn("editor/A.cpp", fan)

    def test_a_header_no_unit_reads_rebuilds_nothing(self):
        fan = check_fanout.fan_out(check_fanout.parse_deps(DEPS, Path("/src/build/linux/editor"), self.root))
        rows = dict((h, e) for h, e, _ in check_fanout.component_rows(fan, self.root))
        self.assertEqual(rows["engine/core/component/Unused.h"], 0)

    def test_limits(self):
        limit = check_fanout.WORST_LIMIT
        typical = check_fanout.TYPICAL_LIMIT
        self.assertEqual(check_fanout.check_limits([("a.h", limit, 0), ("b.h", typical, 0), ("c.h", 1, 0)]), [])
        over = check_fanout.check_limits([("a.h", limit + 1, 0), ("b.h", typical, 0), ("c.h", 1, 0)])
        self.assertEqual(len(over), 1)
        self.assertIn("a.h rebuilds", over[0])
        median = check_fanout.check_limits([("a.h", typical + 1, 0), ("b.h", typical + 1, 0), ("c.h", 1, 0)])
        self.assertEqual(len(median), 1)
        self.assertIn("typical", median[0])


if __name__ == "__main__":
    unittest.main()
