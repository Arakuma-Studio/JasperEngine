// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RmlLogTranslator.h"

#include "SourceFiles.h"

#include "HtmlUIDependencies.h"
#include "Log.h"

#include <algorithm>
#include <map>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    struct Registry {
        std::mutex mutex;
        uint64_t nextHandle = 1;
        std::map<uint64_t, RmlLogTranslator::Registration> registrations;
    };

    Registry& registry(){
        static Registry instance;
        return instance;
    }

    thread_local RmlLogTranslator::Capture* activeCapture = nullptr;

    bool isDigit(char c){
        return c >= '0' && c <= '9';
    }

    bool isSpace(char c){
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    // digits at text[pos...]; false when none
    bool readNumber(std::string_view text, size_t& pos, uint32_t& value){
        const size_t start = pos;
        uint64_t number = 0;
        while (pos < text.size() && isDigit(text[pos])){
            number = number * 10 + (uint64_t)(text[pos] - '0');
            if (number > 0xFFFFFFFFull)
                number = 0xFFFFFFFFull;
            pos++;
        }
        value = (uint32_t)number;
        return pos > start;
    }

    // "<url>:<digits>" or "<url>: <digits>" starting at pos (the url has no whitespace)
    bool readUrlAndLine(std::string_view text, size_t pos, std::string& url, uint32_t& line, size_t& end){
        size_t urlEnd = pos;
        while (urlEnd < text.size() && !isSpace(text[urlEnd]))
            urlEnd++;
        std::string_view token = text.substr(pos, urlEnd - pos);

        // "url:12" (possibly followed by '.' or ',')
        size_t trailing = token.size();
        while (trailing > 0 && (token[trailing - 1] == '.' || token[trailing - 1] == ',' || token[trailing - 1] == ';'))
            trailing--;
        std::string_view core = token.substr(0, trailing);
        size_t digits = core.size();
        while (digits > 0 && isDigit(core[digits - 1]))
            digits--;
        if (digits < core.size() && digits > 1 && core[digits - 1] == ':'){
            url = std::string(core.substr(0, digits - 1));
            size_t numberPos = 0;
            std::string_view number = core.substr(digits);
            readNumber(number, numberPos, line);
            end = pos + trailing;
            return true;
        }

        // "url: 12"
        if (core.size() > 1 && core.back() == ':'){
            size_t next = urlEnd;
            while (next < text.size() && text[next] == ' ')
                next++;
            size_t numberEnd = next;
            if (readNumber(text, numberEnd, line)){
                url = std::string(core.substr(0, core.size() - 1));
                end = numberEnd;
                return true;
            }
        }
        return false;
    }

    std::string trimMessage(std::string text){
        // drop what the removed location leaves behind: " .", trailing spaces, a dangling " :"
        while (!text.empty() && (isSpace(text.back()) || text.back() == ',' || text.back() == ':'))
            text.pop_back();
        size_t pos;
        while ((pos = text.find(" .")) != std::string::npos)
            text.erase(pos, 1);
        while ((pos = text.find("  ")) != std::string::npos)
            text.erase(pos, 1);
        // the diagnostic text has no final period
        while (!text.empty() && (isSpace(text.back()) || text.back() == '.'))
            text.pop_back();
        return text;
    }

    // "declaration 'width: calc(1px+2px);'" -> "width"
    std::string subjectOf(std::string_view message){
        static const char* markers[] = {"declaration '", "property '"};
        for (const char* marker : markers){
            const size_t at = message.find(marker);
            if (at == std::string_view::npos)
                continue;
            const size_t start = at + std::char_traits<char>::length(marker);
            size_t end = start;
            while (end < message.size() && message[end] != ':' && message[end] != '\'' && !isSpace(message[end]))
                end++;
            if (end > start)
                return std::string(message.substr(start, end - start));
        }
        return std::string();
    }

    void logTranslated(const Diagnostic& diagnostic){
        const std::string text = toText(diagnostic);
        if (diagnostic.severity == Severity::Error){
            Log::error("[HTML UI] %s", text.c_str());
            HtmlUIFileLog::recordMessage(HtmlUIFileLog::Level::Error, text);
        }else if (diagnostic.severity == Severity::Warning){
            Log::warn("[HTML UI] %s", text.c_str());
            HtmlUIFileLog::recordMessage(HtmlUIFileLog::Level::Warning, text);
        }else{
            Log::verbose("[HTML UI] %s", text.c_str());
        }
    }

}

uint64_t RmlLogTranslator::add(Registration registration){
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    for (LineMapping& mapping : registration.mappings)
        mapping.url = normalizeUrl(mapping.url);
    const uint64_t handle = reg.nextHandle++;
    reg.registrations.emplace(handle, std::move(registration));
    return handle;
}

void RmlLogTranslator::remove(uint64_t handle){
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    reg.registrations.erase(handle);
}

size_t RmlLogTranslator::registrationCount(){
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    return reg.registrations.size();
}

bool RmlLogTranslator::handle(Rml::Log::Type type, const Rml::String& message){
    Severity severity = Severity::Info;
    if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT)
        severity = Severity::Error;
    else if (type == Rml::Log::LT_WARNING)
        severity = Severity::Warning;
    return handleMessage(severity, message);
}

bool RmlLogTranslator::handleMessage(Severity severity, const std::string& message){
    if (isUnsetSentinel(message))
        return true;

    if (activeCapture){
        activeCapture->collected.push_back(Capture::Message{severity, message});
        return true;
    }

    // infos and debug output stay RmlSystem's
    if (severity == Severity::Info)
        return false;

    const std::optional<MessageLocation> location = findLocation(message);
    if (!location)
        return false;

    std::vector<std::function<void(const Diagnostic&)>> sinks;
    std::optional<Diagnostic> mapped;
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        for (auto& entry : reg.registrations){
            const Registration& registration = entry.second;
            if (!registration.files)
                continue;
            bool named = false;
            for (const LineMapping& mapping : registration.mappings){
                if (mapping.url == location->url){
                    named = true;
                    break;
                }
            }
            if (!named)
                continue;
            std::optional<Diagnostic> diagnostic = translate(message, *registration.files, Span<const LineMapping>(registration.mappings));
            if (!diagnostic)
                continue;
            if (!mapped)
                mapped = diagnostic;
            if (registration.sink)
                sinks.push_back(registration.sink);
        }
    }

    if (!mapped)
        return false;

    logTranslated(*mapped);
    for (const auto& sink : sinks)
        sink(*mapped);
    return true;
}

RmlLogTranslator::Capture::Capture(): previous(activeCapture) {
    activeCapture = this;
}

RmlLogTranslator::Capture::~Capture(){
    activeCapture = previous;
}

size_t RmlLogTranslator::Capture::count(Severity severity) const{
    size_t result = 0;
    for (const Message& message : collected){
        if (message.severity == severity)
            result++;
    }
    return result;
}

bool RmlLogTranslator::isUnsetSentinel(std::string_view message){
    return message.find("Invalid substitution, variable '--html-unset' not defined") != std::string_view::npos;
}

std::string RmlLogTranslator::normalizeUrl(std::string_view url){
    std::string result(url);
    for (char& c : result){
        if (c == '|')
            c = ':';
        else if (c == '\\')
            c = '/';
    }
    while (!result.empty() && result[0] == '/')
        result.erase(0, 1);
    return result;
}

std::optional<RmlLogTranslator::MessageLocation> RmlLogTranslator::findLocation(std::string_view message){
    std::optional<MessageLocation> found;

    // "on line <line> of <url>" (BaseXMLParser)
    {
        const std::string_view marker = "on line ";
        size_t at = message.rfind(marker);
        if (at != std::string_view::npos){
            size_t pos = at + marker.size();
            uint32_t line = 0;
            if (readNumber(message, pos, line) && message.substr(pos, 4) == " of "){
                size_t urlStart = pos + 4;
                size_t urlEnd = urlStart;
                while (urlEnd < message.size() && !isSpace(message[urlEnd]))
                    urlEnd++;
                std::string_view url = message.substr(urlStart, urlEnd - urlStart);
                while (!url.empty() && (url.back() == '.' || url.back() == ','))
                    url.remove_suffix(1);
                if (!url.empty()){
                    MessageLocation location;
                    location.url = normalizeUrl(url);
                    location.line = line;
                    location.begin = at;
                    location.end = urlStart + url.size();
                    found = location;
                }
            }
        }
    }

    // "at <url>:<line>", "At <url>:<line>", "in <url>: <line>", "on <url>:<line>": the last one
    static const std::string_view markers[] = {" at ", "At ", " in ", " on "};
    for (const std::string_view& marker : markers){
        size_t searchFrom = 0;
        while (true){
            const size_t at = message.find(marker, searchFrom);
            if (at == std::string_view::npos)
                break;
            searchFrom = at + 1;
            if (marker == "At " && at != 0 && !isSpace(message[at - 1]) && message[at - 1] != '.')
                continue;
            std::string url;
            uint32_t line = 0;
            size_t end = 0;
            if (!readUrlAndLine(message, at + marker.size(), url, line, end))
                continue;
            if (found && found->begin > at)
                continue;
            MessageLocation location;
            location.url = normalizeUrl(url);
            location.line = line;
            location.begin = (marker[0] == ' ') ? at + 1 : at;
            location.end = end;
            found = location;
        }
    }

    if (found && found->url.empty())
        return std::nullopt;
    return found;
}

std::optional<Diagnostic> RmlLogTranslator::translate(std::string_view message, const SourceFiles& files,
                                                      Span<const LineMapping> mappings){
    const std::optional<MessageLocation> location = findLocation(message);
    if (!location)
        return std::nullopt;

    const LineMapping* best = nullptr;
    SourceRef bestRef;
    for (const LineMapping& mapping : mappings){
        if (!mapping.lines || normalizeUrl(mapping.url) != location->url)
            continue;
        if (location->line < mapping.firstLine)
            continue;
        const size_t index = location->line - mapping.firstLine;
        if (index >= mapping.lines->size())
            continue;
        const SourceRef ref = (*mapping.lines)[index];
        if (!ref.valid())
            continue;
        if (!best || mapping.lines->size() < best->lines->size()){
            best = &mapping;
            bestRef = ref;
        }
    }
    if (!best)
        return std::nullopt;

    std::string text(message.substr(0, location->begin));
    text += message.substr(location->end);
    text = trimMessage(text);

    Diagnostic diagnostic;
    diagnostic.code = codes::RmluiMessage;
    const DiagCodeInfo* info = findDiagnosticCode(codes::RmluiMessage);
    diagnostic.severity = info ? info->severity : Severity::Warning;
    diagnostic.message = "RmlUi: " + text;
    diagnostic.subject = subjectOf(message);
    diagnostic.origin = "RmlUi";

    diagnostic.file = files.path(bestRef.file);
    const SourcePos pos = files.position(bestRef.file, bestRef.offset);
    diagnostic.line = pos.line;
    diagnostic.column = pos.column;
    diagnostic.length = files.codePointLength(bestRef);
    return diagnostic;
}
