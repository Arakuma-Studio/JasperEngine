// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_DIAGNOSTICS_H
#define COMPAT_DIAGNOSTICS_H

#include "Export.h"
#include "CompatTypes.h"
#include "DiagnosticCodes.h"

#include "json.hpp"

#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace jasperengine::htmlui::compat {

    class SourceFiles;

    // One finding of the compatibility layer: something dropped, rewritten with a loss, or
    // unsupported, with the exact location in the user's file (phase-2 §3.4).
    struct Diagnostic {
        Severity severity = Severity::Warning;
        DiagCode code{};
        std::string message;      // one sentence, names the construct: "'border-style: dashed' is drawn solid"
        std::string suggestion;   // what to write instead, "" when none
        std::string replacement;  // exact replacement text for [line:column, +length), "" if not mechanical
        std::string subject;      // property / selector feature / element / at-rule / URL; key into the tables
        std::string file;         // asset-relative with '/', "" when generated
        uint32_t line = 0, column = 0, length = 0;   // 1-based, length in code points; 0 = unknown
        std::string origin;       // "imported by ui/menu.css:3", "inline style", "user-agent stylesheet"
    };

    // Collects the diagnostics of one compile session. Locations come from SourceRefs into
    // `files`, which must outlive the sink.
    class JASPERENGINE_API DiagnosticSink {
    public:
        explicit DiagnosticSink(const SourceFiles& files, size_t limit = 1000);

        // With the registry's default severity. Fills file/line/column/length/origin from
        // ref; drops an identical (code, ref, subject) report.
        void report(DiagCode code, SourceRef ref, std::string message,
                    std::string suggestion = {}, std::string replacement = {}, std::string subject = {});
        // For codes whose severity is "by case" (Appendix F): `severity` is used as given;
        // for the other codes it is replaced by the registry's (a rewrite cannot change it).
        void reportAs(Severity severity, DiagCode code, SourceRef ref, std::string message,
                      std::string suggestion = {}, std::string replacement = {}, std::string subject = {});
        // A diagnostic located by the caller (its file/line/... are kept). The severity rule
        // of reportAs applies. Dropped when identical to one already recorded.
        void report(Diagnostic diagnostic);
        // Same, located at ref (file/line/column/length and, when empty, origin are set from it).
        void report(Diagnostic diagnostic, SourceRef ref);

        // Sorted by file, line, column, code (stable for equal keys).
        const std::vector<Diagnostic>& diagnostics() const;
        std::vector<Diagnostic> take();   // the sorted list; the sink is empty afterwards
        size_t count(Severity severity) const;
        size_t size() const;
        bool truncated() const;           // the limit was reached (one GEN-0002 was added)
        // the number of reports made so far, the dropped ones included (unchanged: nothing
        // was reported in between)
        uint64_t reports() const { return reportCount; }
        const SourceFiles& sourceFiles() const { return files; }

        // file/line/column/length of a ref (Diagnostic location fields), origin too when the
        // file was reached through @import/<link>
        void locate(Diagnostic& diagnostic, SourceRef ref) const;

    private:
        void add(Diagnostic diagnostic, SourceRef ref, bool keyByRef);

        const SourceFiles& files;
        size_t limit;
        bool limitReached = false;
        uint64_t reportCount = 0;
        mutable bool sorted = true;
        mutable std::vector<Diagnostic> list;
        std::unordered_set<std::string> seen;
    };

    // Sorts by file, line, column, code (stable).
    JASPERENGINE_API void sortDiagnostics(std::vector<Diagnostic>& diagnostics);

    // "ui/menu.css:12:5: warning: message (fix: suggestion) [HTMLUI-CSS-0006]"; the unknown
    // location parts are left out ("warning: message [...]" without a file), the message on
    // one line. The same text as phase 4's check --ui and MCP findings.
    JASPERENGINE_API std::string toText(const Diagnostic& diagnostic);
    // {severity, code, name, message, suggestion, replacement, subject, file, line, column,
    //  length, origin}; severity "error"/"warning"/"info", code "HTMLUI-CSS-0006", name the
    // registry's kebab-case name
    JASPERENGINE_API nlohmann::json toJson(const Diagnostic& diagnostic);
    JASPERENGINE_API nlohmann::json toJson(const std::vector<Diagnostic>& diagnostics);

    // Writes errors and warnings one per line with Log::error / Log::warn, prefixed
    // "[HTML UI] "; infos one per line through Log::verbose when verbose is set; then one
    // summary line through Log::print ("[HTML UI] ui/menu.html: 0 errors, 3 warnings,
    // 12 notes").
    JASPERENGINE_API void logDiagnostics(std::string_view documentPath, const std::vector<Diagnostic>& diagnostics, bool verbose);
    // the summary line without the prefix: "ui/menu.html: 0 errors, 3 warnings, 12 notes"
    JASPERENGINE_API std::string diagnosticSummary(std::string_view documentPath, const std::vector<Diagnostic>& diagnostics);

}

#endif //COMPAT_DIAGNOSTICS_H
