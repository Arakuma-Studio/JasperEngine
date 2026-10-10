// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_URLRESOLVER_H
#define COMPAT_URLRESOLVER_H

#include "Export.h"
#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/CompatTypes.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace jasperengine::htmlui::compat {

    class DiagnosticSink;
    class ImportManifest;
    struct ImportEntry;

    enum class UrlKind : uint8_t {
        Empty,          // "" (no resource)
        Relative,       // img/a.png, ../fonts/x.ttf
        RootRelative,   // /ui/a.png, asset://ui/a.png
        EnginePath,     // data://..., lua://... (the engine's other file schemes), kept as written
        Virtual,        // @htmlui/... (compat's own files)
        Remote,         // http(s)://..., //host/...
        Data,           // data:[mime][;base64],...
        Fragment,       // #id
        OtherScheme,    // javascript:, mailto:, tel:, ... (dropped)
    };

    struct ParsedDataUri {
        std::string mimeType;     // lowercase, without parameters; "text/plain" when none
        bool base64 = false;
        std::string data;         // decoded bytes
    };

    // One URL as compat resolves it (phase-2 §3.7.4).
    struct ResolvedUrl {
        UrlKind kind = UrlKind::Empty;
        std::string url;          // as written, trimmed
        std::string assetPath;    // the local file: asset-relative (Relative, RootRelative, Remote with a manifest
                                  // entry, Data: its virtual file), as written for EnginePath; "" otherwise
        // what RML/RCSS get: "asset://ui/a.png" (an asset: RmlUi's JoinPath keeps a scheme, and
        // the engine's file layer, resource packs included, resolves it; a leading '/' would be
        // an absolute filesystem path), "/@htmlui/data/<sha1-12>.png" (virtual files), "#id";
        // "" = dropped
        std::string emitted;
        std::string fragment;     // "#home" of "icons.svg#home" (kept on emitted)
        bool exists = true;       // the local file exists (checked when asked)
        bool outsideAssets = false;   // a relative path climbing above the assets directory
        const ImportEntry* entry = nullptr;   // Remote: the manifest entry (nullptr = not imported)
        std::string mimeType;     // Data
        std::shared_ptr<const std::string> data;   // Data: the decoded bytes; the caller registers
                                                   // VirtualFiles::add(assetPath, data, owner)

        bool dropped() const { return emitted.empty(); }
        bool local() const { return kind == UrlKind::Relative || kind == UrlKind::RootRelative || kind == UrlKind::EnginePath; }
    };

    // Classifies and resolves every URL the HTML or CSS references (phase-2 §3.7.4).
    class JASPERENGINE_API UrlResolver {
    public:
        static constexpr const char* kAssetScheme = "asset://";

        // files: existence checks (FileData when empty; copied); manifest: remote URLs
        // (ImportManifest::get() when null; must outlive the resolver).
        explicit UrlResolver(const FileAccess& files = FileAccess(), const ImportManifest* manifest = nullptr);

        static UrlKind classify(std::string_view url);

        // `url` written in `fromFile` (asset-relative). resourceKind ("image", "stylesheet",
        // "font", "lottie", "svg", "script", "document") names the resource in messages.
        ResolvedUrl resolve(std::string_view url, const std::string& fromFile, std::string_view resourceKind = {},
                            bool checkExists = true) const;
        // Same, reporting at ref (sink may be null): HTMLUI-GEN-0001 for a missing local file
        // (kept), HTMLUI-IMP-0001 for a remote URL without a manifest entry (dropped: the
        // caller applies the fallback of §3.13.6), HTMLUI-IMP-0009 for an entry whose file is
        // missing, HTMLUI-HTML-0023 for other schemes (dropped).
        ResolvedUrl resolve(std::string_view url, const std::string& fromFile, std::string_view resourceKind,
                            SourceRef ref, DiagnosticSink* sink) const;

        const ImportManifest& manifest() const;

        static std::optional<ParsedDataUri> parseDataUri(std::string_view url);
        // png, jpg, gif, bmp, tga, webp, avif, svg, json, ttf, otf, css, txt; "bin" when unknown
        static std::string extensionForMime(std::string_view mimeType);
        // "@htmlui/data/<sha1-12>.<ext>" for decoded data
        static std::string dataPath(const std::string& data, std::string_view mimeType);

        // `relative` against the directory of fromFile, "." and ".." resolved, '/' separators
        static std::string joinPath(const std::string& fromFile, std::string_view relative, bool* outside = nullptr);
        // "." and ".." resolved, '\' -> '/', no leading '/' (outside: a ".." above the root)
        static std::string normalizePath(std::string_view path, bool* outside = nullptr);
        static std::string percentDecode(std::string_view text);

    private:
        FileAccess files;
        const ImportManifest* manifestOverride;
    };

}

#endif //COMPAT_URLRESOLVER_H
