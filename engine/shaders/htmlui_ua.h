// Stand-in for the HTML UI user-agent files the build embeds (the user-agent stylesheet and
// its internal SVGs, engine/core/htmlui/compat/ua). CMake builds generate the real one
// (engine/core/htmlui/compat/ua/embed_ua.cmake), which takes precedence over this file; the
// editor's Apple export writes it here. Without it documents get no user-agent styles.

#pragma once

#include <cstddef>

namespace jasperengine { namespace htmlui { namespace compat { namespace embedded {

struct UaFile { const char* name; const unsigned char* data; size_t size; };

static const UaFile uaFiles[1] = { { nullptr, nullptr, 0 } };
static const size_t uaFileCount = 0;

} } } }
