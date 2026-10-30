# miniz (vendored)

- Upstream: https://github.com/richgel999/miniz, version 3.0.2 (the split sources of the
  repository root, `miniz*.c/.h`, not the release amalgamation), unchanged.
- Copied from OpenEngine's vendored copy of that tag (`Engine/Source/ThirdParty/miniz`);
  `miniz_export.h` is ours (miniz's CMake generates it upstream).
- License: MIT (`LICENSE`).
- Used by the editor only: the Project Hub's zip import (`editor/hub/ProjectImporter.cpp`,
  docs/specs/S27-editor-tools.md R22). Games never link it.
