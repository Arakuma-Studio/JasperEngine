#!/usr/bin/env python3
"""Native crash/overflow/chaining/hang acceptance tests. Every fatal case is a subprocess."""
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--symbolicate", type=Path, required=True)
    parser.add_argument("--expect-asan", action="store_true")
    parser.add_argument("--mode", action="append", help="run selected subprocess modes")
    args = parser.parse_args()
    modes = ["segv", "abort", "terminate", "stack_overflow", "worker_segv", "worker_stack_overflow", "fpe", "guard_worker", "concurrent", "recursive"]
    if os.name == "nt":
        modes.extend(["pure_call", "recursive_abort", "cached_thread_name"])
    else:
        modes.extend(["bus", "chain", "chain_after_return", "recursive_chain", "recursive_chain_return"])
    expected = {"segv": signal.SIGSEGV, "abort": signal.SIGABRT, "terminate": signal.SIGABRT,
                "stack_overflow": signal.SIGSEGV, "worker_segv": signal.SIGSEGV,
                "worker_stack_overflow": signal.SIGSEGV, "fpe": signal.SIGFPE,
                "guard_worker": signal.SIGSEGV, "concurrent": signal.SIGSEGV, "recursive": signal.SIGSEGV, "chain": signal.SIGSEGV, "chain_after_return": signal.SIGSEGV}
    if os.name != "nt":
        expected["bus"] = signal.SIGBUS
    for mode in args.mode or modes + ["hang"]:
        with tempfile.TemporaryDirectory(prefix="je-native-crash-") as root:
            env = dict(os.environ, JASPERENGINE_CRASH_REPORT_DIR=root)
            # Let ASan's previous handler print, then die with the original fatal signal.
            env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") + ":abort_on_error=0:exitcode=77:disable_coredump=1"
            result = subprocess.run([str(args.binary.resolve()), "--mode", mode, "--out", root],
                                    capture_output=True, text=True, timeout=3 if mode.startswith("recursive") else 12, env=env)
            files = list(Path(root).glob("reports/*/" + ("hang-001.txt" if mode == "hang" else "crash.txt")))
            if mode.startswith("recursive"):
                assert result.returncode != 0, result
                if os.name != "nt":
                    assert result.returncode in (-signal.SIGSEGV, 77), result.stderr
                    if mode.startswith("recursive_chain"):
                        assert (Path(root) / "sentinel").is_file(), "recursive fault skipped the saved OS handler"
                        assert (Path(root) / "sentinel").read_text() == "chained\n", result.stderr
                    elif args.expect_asan:
                        assert "AddressSanitizer" in result.stderr, "recursive fault skipped the saved sanitizer handler"
                assert " report=" not in result.stderr, result.stderr
                assert not files, "the recursive test faults before opening its report"
                print(f"PASS native crash: {mode} (immediate termination)")
                continue
            assert len(files) == 1, (mode, result.returncode, result.stderr[-5000:])
            report = files[0].read_text()
            assert len(re.findall(r"^#\d+ ", report, re.M)) >= 3, (mode, report)
            if mode == "hang":
                assert result.returncode == 0, result.stderr
                assert "crashTestStall" in report, report
                assert "recovered after" in result.stderr, result.stderr
            else:
                assert result.returncode != 0, mode
                if sys.platform == "darwin" and mode == "bus":
                    # raise(SIGBUS) interrupts __pthread_kill in this image. Shared
                    # cache data segments must not make another image claim its PC.
                    assert re.search(r"^#0 libsystem_kernel\.dylib\+", report, re.M), (mode, report)
                if mode == "cached_thread_name":
                    assert "thread_name: Cached fault" in report or "thread_name: unknown" in report, report
                if mode == "concurrent":
                    assert "modules:" in report and "core:" in report, report
                    assert (files[0].parent / "log-tail.txt").is_file(), report
                    assert "thread_name: First fault" in report or (os.name == "nt" and "thread_name: unknown" in report), report
                if os.name != "nt":
                    fatal_signals = {expected[mode]}
                    # Darwin can deliver SIGBUS for a pthread stack guard fault.
                    if sys.platform == "darwin" and mode in ("stack_overflow", "worker_stack_overflow"):
                        fatal_signals.add(signal.SIGBUS)
                    reported_signal = int(re.search(r"^signal: (\d+)$", report, re.M)[1])
                    if mode == "chain_after_return":
                        # The first report is the handled SIGTRAP; the later SIGSEGV
                        # must still chain even though the session already reported.
                        assert reported_signal == signal.SIGTRAP, (mode, report)
                        exit_signal = expected[mode]
                    else:
                        assert reported_signal in fatal_signals, (mode, report)
                        exit_signal = reported_signal
                    assert result.returncode in (-exit_signal, 77), (mode, result.returncode, result.stderr)
                for key in ("product", "version", "build_id", "platform", "backend", "reason", "fault_address",
                            "thread", "pc", "sp", "time_since_start_ms", "session"):
                    assert re.search(r"^" + key + ": .+", report, re.M), (mode, key, report)
                tail = (files[0].parent / "log-tail.txt").read_text()
                assert "last line before the crash" in tail or mode == "guard_worker", (mode, tail)
                assert "JASPERENGINE CRASH:" in result.stderr, (mode, result.stderr)
                if args.expect_asan and mode == "segv":
                    assert "AddressSanitizer" in result.stderr, "the previous sanitizer handler did not run"
                    assert result.stderr.index("JASPERENGINE CRASH:") < result.stderr.index("AddressSanitizer"), result.stderr
                if mode in ("chain", "chain_after_return"):
                    assert (Path(root) / "sentinel").read_text() == "chained\n", result.stderr
                if os.name == "nt":
                    assert (files[0].parent / "crash.dmp").stat().st_size > 0
                else:
                    named = subprocess.run([sys.executable, "-I", str(args.symbolicate), str(files[0]),
                                            "--binary-dir", str(args.binary.resolve().parent)],
                                           capture_output=True, text=True, timeout=30)
                    assert named.returncode == 0, (mode, named.stdout, named.stderr)
                    assert "je_crash_posix::" not in named.stdout, named.stdout
                    assert "handleFatalSignal(" not in named.stdout, named.stdout
                    if mode in ("segv", "worker_segv", "guard_worker", "concurrent"):
                        assert sum("=>" in line and "debugSegv(" in line for line in named.stdout.splitlines()) == 1, named.stdout
                    assert "debugSegv" in named.stdout or mode not in ("segv", "worker_segv", "guard_worker"), named.stdout
            print(f"PASS native crash: {mode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
