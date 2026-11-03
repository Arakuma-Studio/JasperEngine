// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef DEBUGOVERLAYRESOURCES_H
#define DEBUGOVERLAYRESOURCES_H

#include <cstddef>
#include <string>

namespace jasperengine {

    // Files embedded in the engine binary under the "engine://" scheme (RmlFile serves them):
    // the debug overlay's document and style sheet, so it needs nothing of the project
    namespace EngineResources {

        constexpr const char* overlayDocument = "engine://debug/overlay.rml";

        // true for "engine://..." (also "engine|//...", RmlUi's form of a document URL)
        bool isEnginePath(const std::string& path);
        // the bytes of an embedded file, or false when there is none of that name
        bool find(const std::string& path, const char*& data, size_t& size);

    }

}

#endif // DEBUGOVERLAYRESOURCES_H
