// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Diagnostics.h"

#include "SourceFiles.h"

#include "Log.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    Severity effectiveSeverity(DiagCode code, Severity requested){
        const DiagCodeInfo* info = findDiagnosticCode(code);
        if (!info)
            return requested;
        return info->severityByCase ? requested : info->severity;
    }

    Severity defaultSeverity(DiagCode code){
        const DiagCodeInfo* info = findDiagnosticCode(code);
        return info ? info->severity : Severity::Warning;
    }

    bool lessByLocation(const Diagnostic& a, const Diagnostic& b){
        return std::tie(a.file, a.line, a.column, a.code) < std::tie(b.file, b.line, b.column, b.code);
    }

    std::string oneLine(const std::string& text){
        std::string result;
        result.reserve(text.size());
        bool lastSpace = false;
        for (char c : text){
            if (c == '\n' || c == '\r' || c == '\t'){
                if (!lastSpace)
                    result += ' ';
                lastSpace = true;
            }else{
                result += c;
                lastSpace = (c == ' ');
            }
        }
        while (!result.empty() && result.back() == ' ')
            result.pop_back();
        return result;
    }

}

DiagnosticSink::DiagnosticSink(const SourceFiles& files, size_t limit): files(files), limit(limit) {
}

void DiagnosticSink::locate(Diagnostic& diagnostic, SourceRef ref) const{
    if (!ref.valid())
        return;
    diagnostic.file = files.path(ref.file);
    const SourcePos pos = files.position(ref.file, ref.offset);
    diagnostic.line = pos.line;
    diagnostic.column = pos.column;
    diagnostic.length = files.codePointLength(ref);
    if (diagnostic.origin.empty())
        diagnostic.origin = files.originText(ref.file);
}

void DiagnosticSink::report(DiagCode code, SourceRef ref, std::string message,
                            std::string suggestion, std::string replacement, std::string subject){
    reportAs(defaultSeverity(code), code, ref, std::move(message), std::move(suggestion), std::move(replacement), std::move(subject));
}

void DiagnosticSink::reportAs(Severity severity, DiagCode code, SourceRef ref, std::string message,
                              std::string suggestion, std::string replacement, std::string subject){
    Diagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.code = code;
    diagnostic.message = std::move(message);
    diagnostic.suggestion = std::move(suggestion);
    diagnostic.replacement = std::move(replacement);
    diagnostic.subject = std::move(subject);
    add(std::move(diagnostic), ref, true);
}

void DiagnosticSink::report(Diagnostic diagnostic){
    add(std::move(diagnostic), SourceRef{}, false);
}

void DiagnosticSink::report(Diagnostic diagnostic, SourceRef ref){
    add(std::move(diagnostic), ref, true);
}

void DiagnosticSink::add(Diagnostic diagnostic, SourceRef ref, bool keyByRef){
    diagnostic.severity = effectiveSeverity(diagnostic.code, diagnostic.severity);
    if (keyByRef)
        locate(diagnostic, ref);

    std::string key = diagnostic.code.id();
    key += '\x1f';
    if (keyByRef && ref.valid()){
        key += std::to_string(ref.file) + ':' + std::to_string(ref.offset) + ':' + std::to_string(ref.length);
    }else{
        key += diagnostic.file + ':' + std::to_string(diagnostic.line) + ':' + std::to_string(diagnostic.column) + '\x1f' + diagnostic.message;
    }
    key += '\x1f';
    key += diagnostic.subject;
    if (!seen.insert(key).second)
        return;

    if (limitReached)
        return;
    if (list.size() >= limit){
        limitReached = true;
        Diagnostic truncated;
        truncated.severity = defaultSeverity(codes::DiagnosticsTruncated);
        truncated.code = codes::DiagnosticsTruncated;
        truncated.message = "more than " + std::to_string(limit) + " diagnostics; the rest were not recorded";
        list.push_back(std::move(truncated));
        sorted = false;
        return;
    }

    list.push_back(std::move(diagnostic));
    sorted = false;
}

const std::vector<Diagnostic>& DiagnosticSink::diagnostics() const{
    if (!sorted){
        std::stable_sort(list.begin(), list.end(), lessByLocation);
        sorted = true;
    }
    return list;
}

std::vector<Diagnostic> DiagnosticSink::take(){
    diagnostics();
    std::vector<Diagnostic> result = std::move(list);
    list.clear();
    seen.clear();
    limitReached = false;
    sorted = true;
    return result;
}

size_t DiagnosticSink::count(Severity severity) const{
    size_t result = 0;
    for (const Diagnostic& diagnostic : list){
        if (diagnostic.severity == severity)
            result++;
    }
    return result;
}

size_t DiagnosticSink::size() const{
    return list.size();
}

bool DiagnosticSink::truncated() const{
    return limitReached;
}

void jasperengine::htmlui::compat::sortDiagnostics(std::vector<Diagnostic>& diagnostics){
    std::stable_sort(diagnostics.begin(), diagnostics.end(), lessByLocation);
}

std::string jasperengine::htmlui::compat::toText(const Diagnostic& diagnostic){
    std::string text;
    if (!diagnostic.file.empty()){
        text += diagnostic.file;
        if (diagnostic.line > 0){
            text += ':' + std::to_string(diagnostic.line);
            if (diagnostic.column > 0)
                text += ':' + std::to_string(diagnostic.column);
        }
        text += ": ";
    }
    text += severityName(diagnostic.severity);
    text += ": ";
    text += oneLine(diagnostic.message);
    if (!diagnostic.suggestion.empty())
        text += " (fix: " + oneLine(diagnostic.suggestion) + ")";
    text += " [" + diagnostic.code.id() + "]";
    return text;
}

nlohmann::json jasperengine::htmlui::compat::toJson(const Diagnostic& diagnostic){
    const DiagCodeInfo* info = findDiagnosticCode(diagnostic.code);
    nlohmann::json json = nlohmann::json::object();
    json["severity"] = severityName(diagnostic.severity);
    json["code"] = diagnostic.code.id();
    json["name"] = info ? info->name : "";
    json["message"] = diagnostic.message;
    json["suggestion"] = diagnostic.suggestion;
    json["replacement"] = diagnostic.replacement;
    json["subject"] = diagnostic.subject;
    json["file"] = diagnostic.file;
    json["line"] = diagnostic.line;
    json["column"] = diagnostic.column;
    json["length"] = diagnostic.length;
    json["origin"] = diagnostic.origin;
    return json;
}

nlohmann::json jasperengine::htmlui::compat::toJson(const std::vector<Diagnostic>& diagnostics){
    nlohmann::json json = nlohmann::json::array();
    for (const Diagnostic& diagnostic : diagnostics)
        json.push_back(toJson(diagnostic));
    return json;
}

std::string jasperengine::htmlui::compat::diagnosticSummary(std::string_view documentPath, const std::vector<Diagnostic>& diagnostics){
    size_t errors = 0, warnings = 0, notes = 0;
    for (const Diagnostic& diagnostic : diagnostics){
        switch (diagnostic.severity){
            case Severity::Error: errors++; break;
            case Severity::Warning: warnings++; break;
            case Severity::Info: notes++; break;
        }
    }
    return std::string(documentPath) + ": " + std::to_string(errors) + (errors == 1 ? " error, " : " errors, ") +
           std::to_string(warnings) + (warnings == 1 ? " warning, " : " warnings, ") +
           std::to_string(notes) + (notes == 1 ? " note" : " notes");
}

void jasperengine::htmlui::compat::logDiagnostics(std::string_view documentPath, const std::vector<Diagnostic>& diagnostics, bool verbose){
    for (const Diagnostic& diagnostic : diagnostics){
        const std::string text = toText(diagnostic);
        switch (diagnostic.severity){
            case Severity::Error:
                Log::error("[HTML UI] %s", text.c_str());
                break;
            case Severity::Warning:
                Log::warn("[HTML UI] %s", text.c_str());
                break;
            case Severity::Info:
                if (verbose)
                    Log::verbose("[HTML UI] %s", text.c_str());
                break;
        }
    }
    const std::string summary = diagnosticSummary(documentPath, diagnostics);
    Log::print("[HTML UI] %s", summary.c_str());
}
