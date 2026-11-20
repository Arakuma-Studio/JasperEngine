// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DIAG_JOURNAL_H
#define DIAG_JOURNAL_H

// The Gameplay Journal (docs/crash-diagnostics.md, docs/specs/shipped-game-diagnostics.md P1): a
// fixed byte ring of one-line game events, formatted when written so a dying process only copies
// bytes. It lands as journal.txt in crash folders (the signal handler, the Windows writer), beside
// hang reports and in user reports; Lua Journal writes and reads it.
//
//   +412.031 f24718 level.start hard=true name=forest-2 seed=42
//
// Separate from the crash ring of log lines: a log storm must not push the game's events out.

#include "Export.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace jasperengine::diag::journal {

    // A field of an entry. Strings print bare when they match [A-Za-z0-9_.:/+-]+, else quoted;
    // verbatim strings print as given (Lua's "<table>")
    struct Field {
        std::string_view key;
        std::variant<std::string_view, int64_t, double, bool> value;
        bool verbatim = false;
    };

    constexpr size_t kMaxEntryBytes = 256;   // the line without its newline; longer ones end in '~'
    constexpr size_t kMaxFields = 12;
    constexpr size_t kMaxKindBytes = 48;

    // Once, before diag::crash::install copies the ring into the handler's state; 0 allocates
    // nothing (every log returns after one relaxed load). Ignored once the handler took the ring.
    JASPERENGINE_API void configure(size_t bytes);
    JASPERENGINE_API bool enabled();
    JASPERENGINE_API size_t capacity();
    // Any thread, no heap allocation: one fetch_add reserve and a memcpy
    JASPERENGINE_API void log(std::string_view kind, std::span<const Field> fields = {});
    // journal.txt as it would be written now: the header, then the whole entries oldest first
    JASPERENGINE_API std::string text();
    // The newest entries (oldest first), each line without its newline
    JASPERENGINE_API std::vector<std::string> entries(size_t newest);
    JASPERENGINE_API void clear();
    // Entries overwritten since the start (or the last clear)
    JASPERENGINE_API uint64_t dropped();

    // Project Settings > Player > Crash Handling > Journal Engine Events: the engine's own
    // engine.* entries (logEngine writes nothing while off)
    JASPERENGINE_API void setEngineEvents(bool on);
    JASPERENGINE_API bool engineEvents();
    JASPERENGINE_API void logEngine(std::string_view kind, std::span<const Field> fields = {});

    // The editor's Console mirror: called with each entry after its "+<time> " (the Console shows
    // the time itself), "f<frame> <kind> ...", on the writing thread
    using Listener = void (*)(std::string_view entry);
    JASPERENGINE_API void setListener(Listener listener);

    // The header's identity (diag::crash::install calls it): product, version, session
    JASPERENGINE_API void setIdentity(std::string_view product, std::string_view version, std::string_view session);
    // the version setIdentity got ("unknown" before)
    JASPERENGINE_API std::string identityVersion();

    namespace detail {
        // What the crash handler copies at install (constant afterwards)
        JASPERENGINE_API char* ring();
        JASPERENGINE_API std::atomic<uint64_t>* cursor();
        JASPERENGINE_API std::atomic<uint64_t>* written();
        // "JasperEngine journal v1" through "capacity:", each line ended by '\n'
        JASPERENGINE_API std::string headerPrefix();
        // The handler took the ring: configure no longer replaces it
        JASPERENGINE_API void markAttached();
    }

}

#endif // DIAG_JOURNAL_H
