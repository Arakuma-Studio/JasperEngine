# earcut.hpp in JasperEngine

- Upstream: https://github.com/mapbox/earcut.hpp, tag `v2.2.4`.
- Vendored: `include/mapbox/earcut.hpp` and `LICENSE` (ISC). No local changes.
- `CMakeLists.txt` is ours: a header-only INTERFACE library, linked only by the Title Art mesher
  (`JASPERENGINE_TITLE_ART_MESHER`, `docs/specs/S86-title-3d.md` §6.12).
- User: `engine/core/titleart/three_d/TitleEarcut.cpp` (3D title caps, outward bevel bands and
  inflate cells).
