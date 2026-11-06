#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Unit tests of tools/ci/smoke_jit_hangs.py (python3 tools/ci/test_smoke_jit_hangs.py; CI's unit job).

The reports are the watchdog's format (docs/crash-diagnostics.md), their stacks cut from the
editor's first open of tools/fixtures/pbr-extended-export on llvmpipe.
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import smoke_jit_hangs  # noqa: E402

HEADER = "JasperEngine hang report v1\nstalled_ms: 3250\nphase: unknown\nbusy_scopes: none\nstack:\n"
JIT = HEADER + """#0 libjasperengine.so+0xb294cd [je_crash_posix::capture(void**, unsigned long)+0xd]
#3 libLLVM.so.20.1+0x1607724 [llvm::ScheduleDAGInstrs::addPhysRegDeps(llvm::SUnit*, unsigned int)+0x154]
#11 libLLVM.so.20.1+0x2e932b2 [llvm::MCJIT::emitObject(llvm::Module*)+0x142]
#14 libLLVM.so.20.1+0x2e02547 [LLVMGetPointerToGlobal+0x17]
#15 libgallium-25.2.8-0ubuntu0.24.04.4.so+0x7a0752
#21 libjasperengine.so+0xa2c5bd [jasperengine::SokolObject::draw(unsigned int, unsigned int, unsigned int)+0xfd]
#26 libjasperengine.so+0x1802c2d [jasperengine::RenderSystem::draw()+0x3d]
job system:
  je-worker-0 (worker): idle
"""
# the same JIT in Mesa 23 (the Ubuntu 22.04 build image), where gallium is inside swrast_dri.so
JIT_MESA23 = HEADER + """#3 libLLVM-15.so.1+0x120b2d1 [llvm::LiveIntervalCalc::calculate(llvm::LiveInterval&, bool)+0x4a1]
#13 libLLVM-15.so.1+0x2952ba0 [LLVMGetPointerToGlobal+0x10]
#14 swrast_dri.so+0x6c3c97
#21 libjasperengine.so+0xc3188e [jasperengine::SokolDrawBatch::holdDraw(jasperengine::SokolObject*, unsigned int, unsigned int, unsigned int)+0x1fe]
#22 libjasperengine.so+0xc31e94 [jasperengine::SokolObject::draw(unsigned int, unsigned int, unsigned int)+0x74]
job system:
  je-worker-0 (worker): idle
"""
# the same draw on a real driver: not the software JIT
NVIDIA = HEADER + """#3 libnvidia-gpucomp.so.595.99.02+0x2cb9c16
#14 libnvidia-glcore.so.595.99.02+0x105513c
#24 libjasperengine.so+0xa2c59d [jasperengine::SokolObject::draw(unsigned int, unsigned int, unsigned int)+0xfd]
"""
# stuck in the engine, with the driver nowhere
ENGINE = HEADER + """#3 libjasperengine.so+0x17eb23e [jasperengine::RenderSystem::drawMesh(unsigned int)+0x1c]
job system:
"""
# llvmpipe's JIT reached from something other than a draw (a program link, on the render thread)
LINK = HEADER + """#3 libLLVM.so.20.1+0x2e932b2 [llvm::MCJIT::emitObject(llvm::Module*)+0x142]
#15 libgallium-25.2.8-0ubuntu0.24.04.4.so+0x7a0752
#21 libjasperengine.so+0xa2d000 [jasperengine::SokolShader::createShader(jasperengine::ShaderData&)+0x10]
"""
# a job thread may be anywhere: only the stack section counts
JIT_ONLY_IN_THE_JOBS = HEADER + """#3 libjasperengine.so+0x17eb23e [jasperengine::RenderSystem::drawMesh(unsigned int)+0x1c]
job system:
  je-worker-0 (worker): libLLVM.so libgallium jasperengine::SokolObject::draw
"""


class SmokeJitHangsTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="smoke-jit-hangs-test-")
        self.root = Path(self.directory.name)
        self.count = 0

    def tearDown(self):
        self.directory.cleanup()

    def log(self, *reports: str | None, recovered: bool = True) -> Path:
        lines = ["some editor output"]
        for report in reports:
            self.count += 1
            path = self.root / f"hang-{self.count:03}.txt"
            if report is not None:
                path.write_text(report)
            lines.append(f"JASPERENGINE HANG: main thread not responding for 3250 ms (phase: unknown, busy: none) report={path}")
        if recovered:
            lines.append("JASPERENGINE HANG: main thread recovered after 8.5 s (unknown)")
        log = self.root / f"console-{self.count}.log"
        log.write_text("\n".join(lines) + "\n")
        return log

    def test_every_report_in_the_jit_passes(self):
        self.assertTrue(smoke_jit_hangs.only_software_jit([self.log(JIT, JIT)]))

    def test_the_jit_of_mesa_23_passes(self):
        self.assertTrue(smoke_jit_hangs.only_software_jit([self.log(JIT_MESA23, JIT_MESA23)]))

    def test_a_real_driver_fails(self):
        self.assertFalse(smoke_jit_hangs.only_software_jit([self.log(NVIDIA)]))

    def test_a_hang_in_the_engine_fails(self):
        self.assertFalse(smoke_jit_hangs.only_software_jit([self.log(ENGINE)]))

    def test_the_jit_outside_a_draw_fails(self):
        self.assertFalse(smoke_jit_hangs.only_software_jit([self.log(LINK)]))

    def test_one_other_report_among_them_fails(self):
        self.assertFalse(smoke_jit_hangs.only_software_jit([self.log(JIT, ENGINE)]))

    def test_the_job_threads_do_not_count(self):
        self.assertFalse(smoke_jit_hangs.only_software_jit([self.log(JIT_ONLY_IN_THE_JOBS)]))

    def test_a_missing_report_fails(self):
        self.assertFalse(smoke_jit_hangs.only_software_jit([self.log(None)]))

    def test_no_report_at_all_fails(self):
        # (a recovery line alone, or no hang: nothing says it was the JIT)
        self.assertFalse(smoke_jit_hangs.only_software_jit([self.log()]))

    def test_the_command_line_answers_by_status(self):
        self.assertEqual(smoke_jit_hangs.main([str(self.log(JIT))]), 0)
        self.assertEqual(smoke_jit_hangs.main([str(self.log(ENGINE))]), 1)


if __name__ == "__main__":
    unittest.main()
