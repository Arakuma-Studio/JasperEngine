# googletest in JasperEngine

- Upstream: https://github.com/google/googletest, tag `v1.17.0` (BSD-3-Clause, `LICENSE`).
  Release tarball sha256 `65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c`.
- Used by: the unit-test targets only (`jasperengine-core-tests`, `jasperengine-editor-tests`,
  `jasperengine-htmlui-tests`, `jasperengine-htmlui-editor-tests`; see `docs/testing.md`). Tests
  only: it is never linked into the editor, the engine or an exported game, and is not installed.
- Added to a build by `engine/cmake/JasperTesting.cmake` (`EXCLUDE_FROM_ALL`, warnings off), with
  googlemock enabled.

## Vendored (byte for byte from the tag)
`CMakeLists.txt`, `LICENSE`, `README.md`, `CONTRIBUTORS`, and for `googletest/` and `googlemock/`
each: `CMakeLists.txt`, `README.md`, `cmake/`, `include/`, `src/`.

Not vendored: `docs/`, `ci/`, `.github/`, the Bazel files, `googletest/samples/` and both
`test/` directories (googletest's own tests; `gtest_build_tests`/`gmock_build_tests` stay OFF).

## Ours
Nothing is patched. Updating: replace the directories listed above from the new tag, update the
version here and in `NOTICE.md`.
