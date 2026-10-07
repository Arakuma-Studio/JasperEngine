// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RmlSystem.h"

#include "HtmlUIClock.h"
#include "HtmlUIDependencies.h"
#include "Engine.h"
#include "Log.h"

#include <RmlUi/Core/StringUtilities.h>

#include <vector>

using namespace jasperengine;

namespace {

    bool isAbsoluteOrUrl(const std::string& path){
        if (path.find("://") != std::string::npos)
            return true;
        if (!path.empty() && (path[0] == '/' || path[0] == '\\'))
            return true;
        // Windows drive letter: "C:/..." or "C:\..."
        if (path.size() >= 2 && path[1] == ':' &&
            ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')))
            return true;
        return false;
    }

    // resolves "." and ".." like FileData::simplifyPath, always with '/'; ".." above the
    // root is dropped; an absolute path stays absolute
    std::string simplifyPath(const std::string& path){
        const bool absolute = !path.empty() && path[0] == '/';
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= path.size()){
            size_t end = path.find('/', start);
            if (end == std::string::npos)
                end = path.size();

            const std::string part = path.substr(start, end - start);
            if (part == ".."){
                if (!parts.empty())
                    parts.pop_back();
            }else if (!part.empty() && part != "."){
                parts.push_back(part);
            }

            start = end + 1;
        }

        std::string result = absolute ? "/" : "";
        for (size_t i = 0; i < parts.size(); i++){
            if (i > 0)
                result += '/';
            result += parts[i];
        }
        return result;
    }

}

double RmlSystem::GetElapsedTime(){
    return HtmlUIClock::now();
}

bool RmlSystem::LogMessage(Rml::Log::Type type, const Rml::String& message){
    switch (type){
        case Rml::Log::LT_ERROR:
        case Rml::Log::LT_ASSERT:
            errorCount++;
            Log::error("[HTML UI] %s", message.c_str());
            break;
        case Rml::Log::LT_WARNING:
            warningCount++;
            Log::warn("[HTML UI] %s", message.c_str());
            break;
        case Rml::Log::LT_DEBUG:
            Log::debug("[HTML UI] %s", message.c_str());
            break;
        case Rml::Log::LT_ALWAYS:
        case Rml::Log::LT_INFO:
        default:
            Log::verbose("[HTML UI] %s", message.c_str());
            break;
    }

    if (logListener)
        logListener(type, message);

    // the Inspector's per-document messages (UIDocumentSystem::getDocumentMessages)
    if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT){
        HtmlUIFileLog::recordMessage(HtmlUIFileLog::Level::Error, message);
    }else if (type == Rml::Log::LT_WARNING){
        HtmlUIFileLog::recordMessage(HtmlUIFileLog::Level::Warning, message);
    }

    // never break into a debugger
    return true;
}

std::string RmlSystem::joinPath(const std::string& documentPath, const std::string& path){
    if (isAbsoluteOrUrl(path))
        return path;

    // RmlUi hands document paths over with ':' replaced by '|' (StreamFile::Open)
    std::string document = Rml::StringUtilities::Replace(documentPath, '|', ':');
    document = Rml::StringUtilities::Replace(document, '\\', '/');

    std::string scheme;
    const size_t schemeEnd = document.find("://");
    if (schemeEnd != std::string::npos){
        scheme = document.substr(0, schemeEnd + 3);
        document = document.substr(schemeEnd + 3);
    }

    std::string baseDir;
    const size_t lastSlash = document.rfind('/');
    if (lastSlash != std::string::npos)
        baseDir = document.substr(0, lastSlash + 1);

    return scheme + simplifyPath(baseDir + Rml::StringUtilities::Replace(path, '\\', '/'));
}

void RmlSystem::JoinPath(Rml::String& translated_path, const Rml::String& document_path, const Rml::String& path){
    translated_path = joinPath(document_path, path);
}

void RmlSystem::SetClipboardText(const Rml::String& text){
    clipboard = text;
}

void RmlSystem::GetClipboardText(Rml::String& text){
    text = clipboard;
}

void RmlSystem::setLogListener(std::function<void(Rml::Log::Type, const std::string&)> listener){
    logListener = listener;
}

size_t RmlSystem::getErrorCount() const{
    return errorCount;
}

size_t RmlSystem::getWarningCount() const{
    return warningCount;
}

void RmlSystem::resetCounters(){
    errorCount = 0;
    warningCount = 0;
}
