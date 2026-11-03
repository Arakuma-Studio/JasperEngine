// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The precompiled header of our own C++ targets (JASPERENGINE_PCH, engine/cmake/JasperPch.cmake,
// docs/testing.md "Build speed"): the standard headers nearly every unit includes, which cost
// ~1.25 s to parse in each of them (docs/specs/S33-build-speed.md M4). Only headers that never
// change go here: one of ours would make every unit depend on it. Sources still include what
// they use; the build without a precompiled header (every export, web, Android) checks that.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
