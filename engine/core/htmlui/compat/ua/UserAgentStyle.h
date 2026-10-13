// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_UA_USERAGENTSTYLE_H
#define COMPAT_UA_USERAGENTSTYLE_H

#include "Export.h"
#include "htmlui/compat/CompatTypes.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/ForkFeatures.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    // The user-agent stylesheet (phase-2 §3.10): compat/ua/useragent.css, standard CSS,
    // embedded at build time with the internal SVGs next to it (CMake: embed_ua.cmake; Apple
    // exports: buildEmbeddedHeader()), rewritten once per process and fork-feature set into
    // the virtual file every document links first.
    class JASPERENGINE_API UserAgentStyle {
    public:
        // the source's name in SourceFiles and diagnostics
        static constexpr const char* kSourcePath = "@htmlui/useragent.css";

        struct EmbeddedFile {
            const char* name;              // "useragent.css", "arrow-down.svg"
            const unsigned char* data;
            size_t size;
        };
        // every embedded file of compat/ua (sorted by name); empty in a build without them
        static Span<const EmbeddedFile> embeddedFiles();
        static std::string_view source();  // useragent.css ("" when not embedded)

        struct Sheet {
            std::string rcss;
            RcssLineMap lines;
            std::vector<Diagnostic> diagnostics;   // of the rewrite (the UA sheet must have none: p2-ua's test)
            std::string virtualPath;               // "@htmlui/ua.rcss" for ForkFeatures::present()
            ForkFeatures fork;
            std::shared_ptr<const SourceFiles> files;   // the source, for RmlLogTranslator
        };
        // The rewritten sheet for a fork-feature set, computed once, registered in VirtualFiles
        // (process lifetime) and, for RmlUi's messages about it, in RmlLogTranslator.
        static std::shared_ptr<const Sheet> get(const ForkFeatures& fork = ForkFeatures::present());
        // "@htmlui/ua.rcss" for present(), "@htmlui/ua-<bits hex>.rcss" for other sets (tests
        // forcing features: RmlUi caches sheets by path)
        static std::string virtualPath(const ForkFeatures& fork);
        // "@htmlui/ua/<name>" for every embedded file except the stylesheet (arrow-down.svg, ...),
        // process lifetime; idempotent
        static void registerInternalFiles();
        // drops the rewritten sheets (tests; the next get() rewrites again)
        static void clearCache();

        // The embedded header (format of embed_ua.cmake) for these files: the editor's Apple
        // export writes it, whose Xcode project has no embed step.
        static std::string buildEmbeddedHeader(Span<const EmbeddedFile> files);
        static std::string buildEmbeddedHeader();   // of embeddedFiles()
    };

}

#endif //COMPAT_UA_USERAGENTSTYLE_H
