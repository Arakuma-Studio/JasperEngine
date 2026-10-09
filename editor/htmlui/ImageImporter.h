// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ContentSniffer.h"
#include "RemoteImporter.h"

#include <string>

namespace jasperengine::editor::htmlui {

    // Remote images (phase-2 §3.13.3): any http(s) image URL, typed by its magic bytes and
    // Content-Type (PNG, JPEG, GIF, BMP, TGA, SVG text); saved as
    // images/<host>/<sha1-12>-<basename>.<ext> with the extension of the detected type (URLs
    // like images.unsplash.com/photo-…?w=800 and placehold.co/600x400 have none). WebP and
    // AVIF are saved with HTMLUI-IMP-0003 (the engine cannot decode them). License unknown:
    // HTMLUI-IMP-0008.
    class ImageImporter {
    public:
        explicit ImageImporter(RemoteImporter& importer);

        ImportedResource run(const std::string& url, const std::string& finalUrl, const std::string& body,
                             const SniffedContent& sniffed, const ImportLocation& at);

        static std::string localPath(const RemoteImporter& importer, const std::string& url, const SniffedContent& sniffed);

    private:
        RemoteImporter& importer;
    };

}
