// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The precompiled header of the editor's targets (JASPERENGINE_PCH, engine/cmake/JasperPch.cmake):
// the engine's standard headers, plus nlohmann's json.hpp (281 editor units include it, 1.8 s
// each) and yaml-cpp (every unit that includes Project.h, 1.1 s); docs/specs/S33-build-speed.md M4.
// No editor or engine header of ours.

#include "JasperPch.h"
#include "json.hpp"
#include "yaml-cpp/yaml.h"
