// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R58)
// - Purpose: the operating system's font faces with outlines, for the picker's System section.
//   Linux (and other fontconfig systems): `fc-list --format '%{file}\t%{index}\t%{family}\t
//   %{style}\t%{fontformat}\n'` through CommandRunner::capture (no shell, 5 s timeout), keeping
//   TrueType and CFF outlines and dropping bitmap formats and variable-font named instances (the
//   picker's Style row offers those). macOS: CoreText (CTFontManagerCopyAvailableFontURLs, faces
//   from CTFontManagerCreateFontDescriptorsFromURL; SystemFontListMac.mm). Windows: DirectWrite
//   (the system font collection, each face's file through IDWriteLocalFontFileLoader).
// - Lifetime: listed once per session on the jobs' Background lane when the picker first asks
//   (request()); state() and faces() are read from the main thread. Shares nothing with the
//   editor's CJK UI font lookup (editor/util/SystemFonts) except its loadability check, which
//   the import uses.
// - Thread-safety: request/state/faces from the main thread; the listing runs on a worker and
//   hands its result over under a mutex.

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::titlefonts {

    enum class SystemFontFormat : uint8_t { TrueType, Cff };

    struct SystemFontFace {
        std::string path;           // absolute, UTF-8
        int faceIndex = 0;          // the member of a .ttc
        std::string family;         // the first (English) family name
        std::string style;          // the first style name ("Regular", "Bold Italic")
        SystemFontFormat format = SystemFontFormat::TrueType;
    };

    // fc-list output in the format above: one face per line; families and styles that list
    // several names (comma-separated, fontconfig's localized names) keep the first; formats other
    // than TrueType and CFF, named instances (index >= 65536) and repeated (file, index) dropped;
    // sorted by family, then style
    std::vector<SystemFontFace> parseFcList(std::string_view output);

    class SystemFontList {
    public:
        enum class State { Idle, Listing, Ready };
        using Lister = std::function<std::vector<SystemFontFace>()>;

        // the editor's list (one per session)
        static SystemFontList& instance();

        SystemFontList();
        ~SystemFontList();
        SystemFontList(const SystemFontList&) = delete;
        SystemFontList& operator=(const SystemFontList&) = delete;

        // main thread: starts the listing on the Background lane the first time (later calls do
        // nothing)
        void request();
        State state() const;
        // the faces once Ready (empty before)
        std::vector<SystemFontFace> faces() const;

        // the platform's lister, blocking (fontconfig, CoreText or DirectWrite)
        static std::vector<SystemFontFace> listPlatform();

        // tests: the lister a request runs (an empty function restores listPlatform), and back to Idle
        void setLister(Lister lister);
        void reset();

    private:
        struct Shared;
        std::shared_ptr<Shared> shared;
        Lister lister;
    };

#if defined(__APPLE__)
    // SystemFontListMac.mm
    std::vector<SystemFontFace> listCoreTextFonts();
#endif

}
