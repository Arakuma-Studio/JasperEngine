// Stand-in for the built-in Lucide icon pack (engine/libs/lucide/lucide.pack). CMake builds
// generate the real one (engine/libs/lucide/embed_lucide.cmake), which takes precedence over
// this file; the editor's Apple export writes it here. Without it data-lucide icons are
// reported unknown (HTMLUI-HTML-0021).

#pragma once

#include <cstddef>

namespace jasperengine { namespace htmlui { namespace compat { namespace embedded {

static const unsigned char lucidePack[] = {0x00};
static const size_t lucidePackSize = 0;

} } } }
