# Clipper2 in JasperEngine

- Upstream: https://github.com/AngusJohnson/Clipper2, tag `Clipper2_2.0.1` (2025-12-19).
- Vendored: `CPP/Clipper2Lib/include/clipper2/*.h` as `include/clipper2/`, `CPP/Clipper2Lib/src/*.cpp`
  as `src/`, and `LICENSE` (Boost Software License 1.0). No local changes.
- `CMakeLists.txt` is ours: one static library, hidden visibility, built only with
  `JASPERENGINE_TITLE_ART` (`docs/specs/S80-title-art-core.md` §6.15).
- User: `engine/core/titleart/geometry/Offset.cpp` (face weight, plate closing, stroke outlines).
