# Noise: measurements

Budgets of `docs/specs/T2-noise-graphs.md` R46-R49, gated by `je-bench-noise` and `tools/perf/check_bench.py`
on an idle machine. Numbers below are from the run named on each table.

## Determinism record

One line per platform, from `jasperengine-noise-golden --all-backends --report` (R72). A platform without a
row is **unverified**. A mismatch is a kernel bug, never a new table.

| Platform | Report | Who, when |
|---|---|---|
| Linux x86_64, GCC (the Linux build image) | `linux-x86_64 sse2 142 pass`, `scalar 142 pass`, table `80C376F5562F414C` | T2, 2026-11-07, this host |
| Android x86_64, NDK clang 28.2, static bionic | `android-x86_64 sse2 142 pass`, `scalar 142 pass`, table `80C376F5562F414C` | T2, 2026-11-07, `tools/noise/cross_check.sh` in the Android image |
| Android x86, NDK clang 28.2, static bionic | `android-x86 sse2 142 pass`, `scalar 142 pass`, table `80C376F5562F414C` | T2, 2026-11-07, `cross_check.sh` |
| Android arm64-v8a, NDK clang 28.2, static bionic, **under qemu-aarch64-static (emulation, not ARM hardware)** | `android-arm64-v8a neon 142 pass`, `scalar 142 pass`, table `80C376F5562F414C` | T2, 2026-11-07, `cross_check.sh` |
| armv7 (armeabi-v7a's ISA and flags), Ubuntu's arm-linux-gnueabihf-g++, static glibc, **under qemu-arm-static (emulation)**; NDK clang's armv7 build unverified: its static bionic would not start under qemu in the container | `linux-arm scalar 142 pass`, table `80C376F5562F414C` | T2, 2026-11-07, `cross_check.sh` |

`cross_check.sh --red-contract` (x86_64 with `-ffp-contract=fast -mfma`) fails the table, as it must: 238
mismatches over the two back ends, in kernel, fractal, math, node, erosion, preset and compat cases.
