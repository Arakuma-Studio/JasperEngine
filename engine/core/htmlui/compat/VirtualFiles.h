// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_VIRTUALFILES_H
#define COMPAT_VIRTUALFILES_H

#include "Export.h"
#include "htmlui/compat/CompatOptions.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    // In-memory files under "@htmlui/" (phase-2 §3.2): the rewritten user-agent sheet
    // (@htmlui/ua.rcss), the user-agent's internal SVGs (@htmlui/ua/*.svg), rewritten author
    // sheets (@htmlui/doc/<key>/<asset path>.rcss) and decoded data: URIs
    // (@htmlui/data/<sha1-12>.<ext>). RmlFile (hook H3b) and RmlRenderer::LoadTexture (H7)
    // serve these paths before touching the file system; a path may be written with a
    // leading '/' (RML/RCSS emit "/@htmlui/..." so RmlUi's JoinPath keeps them).
    // Thread-safe. Files of a compile belong to an owner (the compile key) and go with it;
    // owner 0 = the process lifetime.
    class JASPERENGINE_API VirtualFiles {
    public:
        static constexpr const char* kPrefix = "@htmlui/";

        static bool isVirtual(std::string_view path);           // "@htmlui/..." or "/@htmlui/..."
        static std::string normalize(std::string_view path);    // without the leading '/'

        static void add(std::string_view path, std::string content, uint64_t owner = 0);
        static void add(std::string_view path, std::shared_ptr<const std::string> content, uint64_t owner = 0);
        static bool remove(std::string_view path);
        static size_t removeOwner(uint64_t owner);              // files removed
        static std::shared_ptr<const std::string> find(std::string_view path);
        static bool exists(std::string_view path);
        static std::vector<std::string> list(std::string_view prefix = kPrefix);
        static size_t count();
        static void clear();                                     // tests
    };

    // Reads an asset-relative file: virtual files first, then `access` (FileData when empty).
    JASPERENGINE_API bool readAssetFile(const FileAccess& access, const std::string& path, std::string& content);
    JASPERENGINE_API bool assetFileExists(const FileAccess& access, const std::string& path);

}

#endif //COMPAT_VIRTUALFILES_H
