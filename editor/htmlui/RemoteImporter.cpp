// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RemoteImporter.h"

#include "ContentSniffer.h"
#include "GoogleFontsImporter.h"
#include "ImageImporter.h"
#include "ImportManifestWriter.h"
#include "LottieImporter.h"
#include "StylesheetImporter.h"
#include "VideoImporter.h"

#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/Hash.h"
#include "htmlui/compat/UrlResolver.h"

#include "yaml-cpp/yaml.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor::htmlui;
using jasperengine::htmlui::compat::ImportEntry;
using jasperengine::htmlui::compat::ImportManifest;
using jasperengine::htmlui::compat::UrlKind;
using jasperengine::htmlui::compat::UrlResolver;
namespace fs = std::filesystem;

namespace {

    bool readFile(const fs::path& path, std::string& content){
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;
        std::ostringstream stream;
        stream << file.rdbuf();
        content = stream.str();
        return true;
    }

    struct CollectorState {
        std::mutex mutex;
        ReferenceCollector collector;
    };

    CollectorState& collectorState(){
        static CollectorState state;
        return state;
    }

    // "." and ".." of a '/' path resolved (a ".." above the root is dropped)
    std::string removeDotSegments(const std::string& path){
        std::vector<std::string> segments;
        size_t start = 0;
        const bool absolute = !path.empty() && path[0] == '/';
        bool trailing = false;
        while (start <= path.size()){
            size_t end = path.find('/', start);
            if (end == std::string::npos)
                end = path.size();
            const std::string segment = path.substr(start, end - start);
            trailing = false;
            if (segment == ".."){
                if (!segments.empty())
                    segments.pop_back();
                trailing = true;
            }else if (segment == "."){
                trailing = true;
            }else if (!segment.empty()){
                segments.push_back(segment);
            }else if (end == path.size() && end > 0){
                trailing = true;
            }
            start = end + 1;
        }
        std::string out = absolute ? "/" : "";
        for (size_t i = 0; i < segments.size(); i++){
            if (i > 0)
                out += '/';
            out += segments[i];
        }
        if (trailing && !segments.empty())
            out += '/';
        return out;
    }

}

// ---------------------------------------------------------------------------------------------
// ImportTarget
// ---------------------------------------------------------------------------------------------

bool ImportTarget::forProject(const fs::path& projectRoot, const std::string& destDir, ImportTarget& target, std::string& error){
    std::error_code ec;
    target = ImportTarget();
    target.projectRoot = fs::absolute(projectRoot, ec).lexically_normal();
    if (target.projectRoot.filename() == "project.yaml")
        target.projectRoot = target.projectRoot.parent_path();
    if (!target.projectRoot.empty() && target.projectRoot.filename().empty())
        target.projectRoot = target.projectRoot.parent_path();

    std::string assetsDir;
    if (fs::is_regular_file(target.projectRoot / "project.yaml", ec)){
        try {
            YAML::Node node = YAML::LoadFile((target.projectRoot / "project.yaml").string());
            if (node["assetsDir"])
                assetsDir = node["assetsDir"].as<std::string>();
        } catch (const std::exception&) {
        }
    }
    const fs::path dir = editor::FileUtils::pathFromUtf8(assetsDir);
    if (dir.empty() || dir == ".")
        target.assetsDir = target.projectRoot;
    else if (dir.is_absolute())
        target.assetsDir = dir.lexically_normal();
    else
        target.assetsDir = (target.projectRoot / dir).lexically_normal();
    if (!target.assetsDir.empty() && target.assetsDir.filename().empty())
        target.assetsDir = target.assetsDir.parent_path();

    if (destDir.empty())
        return true;
    fs::path dest = editor::FileUtils::pathFromUtf8(destDir);
    if (dest.is_relative())
        dest = target.projectRoot / dest;
    dest = dest.lexically_normal();
    const std::string relative = target.toAssetPath(dest);
    if (relative.empty() && dest != target.assetsDir){
        error = "the destination '" + destDir + "' is not inside the project's assets directory (" +
                target.toProjectRelative("") + ")";
        return false;
    }
    target.root = relative;
    return true;
}

std::string ImportTarget::toProjectRelative(const std::string& assetPath) const{
    const fs::path absolute = assetPath.empty() ? assetsDir : assetsDir / editor::FileUtils::pathFromUtf8(assetPath);
    const fs::path relative = absolute.lexically_relative(projectRoot);
    if (relative.empty())
        return editor::FileUtils::pathToGenericUtf8(absolute);
    if (relative == ".")
        return ".";
    if (*relative.begin() == "..")
        return editor::FileUtils::pathToGenericUtf8(absolute);
    return editor::FileUtils::pathToGenericUtf8(relative);
}

fs::path ImportTarget::toAbsolute(const std::string& assetPath) const{
    return (assetsDir / editor::FileUtils::pathFromUtf8(assetPath)).lexically_normal();
}

std::string ImportTarget::toAssetPath(const fs::path& path) const{
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec).lexically_normal();
    const fs::path relative = absolute.lexically_relative(assetsDir);
    if (relative.empty() || relative == "." || *relative.begin() == "..")
        return "";
    std::string text = editor::FileUtils::pathToGenericUtf8(relative);
    while (!text.empty() && text.back() == '/')
        text.pop_back();
    return text;
}

compat::FileAccess ImportTarget::fileAccess() const{
    compat::FileAccess access;
    const fs::path root = assetsDir;
    access.read = [root](const std::string& assetPath, std::string& content){
        return readFile(root / editor::FileUtils::pathFromUtf8(assetPath), content);
    };
    access.exists = [root](const std::string& assetPath){
        std::error_code ec;
        return fs::is_regular_file(root / editor::FileUtils::pathFromUtf8(assetPath), ec);
    };
    return access;
}

// ---------------------------------------------------------------------------------------------
// ImportStaging
// ---------------------------------------------------------------------------------------------

void ImportStaging::add(const std::string& assetPath, std::string content){
    pending[assetPath] = std::move(content);
}

bool ImportStaging::has(const std::string& assetPath) const{
    return pending.count(assetPath) != 0;
}

bool ImportStaging::commit(const ImportTarget& target, std::vector<std::string>& written, std::string& error) const{
    for (const auto& pair : pending){
        if (!ImportManifestWriter::writeFileAtomic(target.toAbsolute(pair.first), pair.second, &error))
            return false;
        written.push_back(pair.first);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------------

std::string jasperengine::editor::htmlui::lowerAscii(std::string text){
    for (char& c : text){
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }
    return text;
}

bool jasperengine::editor::htmlui::splitUrl(const std::string& url, std::string& host, std::string& path, std::string& query){
    const std::string normalized = ImportManifest::normalizeUrl(url);
    if (normalized.compare(0, 8, "https://") != 0)
        return false;
    const std::string rest = normalized.substr(8);
    const size_t slash = rest.find_first_of("/?");
    host = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string remainder = slash == std::string::npos ? "/" : rest.substr(slash);
    const size_t question = remainder.find('?');
    path = question == std::string::npos ? remainder : remainder.substr(0, question);
    query = question == std::string::npos ? "" : remainder.substr(question + 1);
    if (path.empty())
        path = "/";
    return !host.empty();
}

std::string jasperengine::editor::htmlui::resolveUrl(const std::string& base, const std::string& reference){
    std::string ref = reference;
    // trim
    while (!ref.empty() && (unsigned char)ref.front() <= ' ')
        ref.erase(0, 1);
    while (!ref.empty() && (unsigned char)ref.back() <= ' ')
        ref.pop_back();
    if (ref.empty())
        return base;

    // absolute reference
    const size_t colon = ref.find(':');
    const size_t firstSpecial = ref.find_first_of("/?#");
    if (colon != std::string::npos && (firstSpecial == std::string::npos || colon < firstSpecial))
        return ref;

    const size_t schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos)
        return ref;
    const std::string scheme = base.substr(0, schemeEnd);
    if (ref.compare(0, 2, "//") == 0)
        return scheme + ":" + ref;

    const size_t authorityEnd = base.find_first_of("/?#", schemeEnd + 3);
    const std::string origin = authorityEnd == std::string::npos ? base : base.substr(0, authorityEnd);
    std::string basePath = authorityEnd == std::string::npos ? "/" : base.substr(authorityEnd);
    const size_t fragment = basePath.find('#');
    if (fragment != std::string::npos)
        basePath.erase(fragment);
    std::string baseQuery;
    const size_t question = basePath.find('?');
    if (question != std::string::npos){
        baseQuery = basePath.substr(question);
        basePath.erase(question);
    }
    if (basePath.empty() || basePath[0] != '/')
        basePath.insert(0, "/");

    if (ref[0] == '#')
        return origin + basePath + baseQuery + ref;
    if (ref[0] == '?')
        return origin + basePath + ref;

    std::string refPath = ref;
    std::string suffix;
    const size_t refCut = refPath.find_first_of("?#");
    if (refCut != std::string::npos){
        suffix = refPath.substr(refCut);
        refPath.erase(refCut);
    }
    std::string merged;
    if (!refPath.empty() && refPath[0] == '/')
        merged = refPath;
    else
        merged = basePath.substr(0, basePath.find_last_of('/') + 1) + refPath;
    return origin + removeDotSegments(merged) + suffix;
}

std::string jasperengine::editor::htmlui::safeFileName(const std::string& segment){
    std::string name = UrlResolver::percentDecode(segment);
    std::string out;
    for (unsigned char c : name){
        if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
            c == '*' || c == ' ' || c == '%' || c == '#' || c == '&' || c == '=' || c == '+')
            out += '_';
        else
            out += (char)c;
    }
    while (!out.empty() && (out.back() == '.' || out.back() == '_'))
        out.pop_back();
    while (!out.empty() && out.front() == '.')
        out.erase(0, 1);
    if (out.size() > 80)
        out.resize(80);
    return out;
}

std::string jasperengine::editor::htmlui::relativeAssetPath(const std::string& fromFile, const std::string& target){
    const size_t slash = fromFile.find_last_of('/');
    const fs::path fromDir = slash == std::string::npos ? fs::path(".") : editor::FileUtils::pathFromUtf8(fromFile.substr(0, slash));
    const fs::path relative = editor::FileUtils::pathFromUtf8(target).lexically_relative(fromDir);
    if (relative.empty())
        return target;
    return editor::FileUtils::pathToGenericUtf8(relative);
}

std::string jasperengine::editor::htmlui::urlHash(const std::string& url, size_t digits){
    return compat::sha1Hex(ImportManifest::normalizeUrl(url)).substr(0, digits);
}

bool jasperengine::editor::htmlui::locateText(const std::string& text, const std::string& needle, uint32_t& line, uint32_t& column){
    if (needle.empty())
        return false;
    const size_t at = text.find(needle);
    if (at == std::string::npos)
        return false;
    line = 1;
    size_t lineStart = 0;
    for (size_t i = 0; i < at; i++){
        if (text[i] == '\n' || (text[i] == '\r' && (i + 1 >= text.size() || text[i + 1] != '\n'))){
            line++;
            lineStart = i + 1;
        }
    }
    column = 1;
    for (size_t i = lineStart; i < at; i++){
        if (((unsigned char)text[i] & 0xC0) != 0x80)
            column++;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// discovery
// ---------------------------------------------------------------------------------------------

void jasperengine::editor::htmlui::setReferenceCollector(ReferenceCollector collector){
    std::lock_guard<std::mutex> lock(collectorState().mutex);
    collectorState().collector = std::move(collector);
}

std::vector<DiscoveredReference> jasperengine::editor::htmlui::discoverRemoteReferences(const ImportTarget& target, const std::string& assetPath){
    ReferenceCollector collector;
    {
        std::lock_guard<std::mutex> lock(collectorState().mutex);
        collector = collectorState().collector;
    }
    const compat::FileAccess files = target.fileAccess();
    const std::vector<compat::RemoteReference> references = collector ? collector(assetPath, files)
                                                                      : compat::collectRemoteReferences(assetPath, files);

    std::string text;
    files.read(assetPath, text);

    std::vector<DiscoveredReference> result;
    std::set<std::string> seen;
    for (const compat::RemoteReference& reference : references){
        if (UrlResolver::classify(reference.url) != UrlKind::Remote)
            continue;
        if (!seen.insert(ImportManifest::normalizeUrl(reference.url)).second)
            continue;
        DiscoveredReference discovered;
        discovered.url = reference.url;
        discovered.kind = reference.kind;
        discovered.location.file = assetPath;
        locateText(text, reference.url, discovered.location.line, discovered.location.column);
        result.push_back(std::move(discovered));
    }
    return result;
}

// ---------------------------------------------------------------------------------------------
// RemoteImporter
// ---------------------------------------------------------------------------------------------

RemoteImporter::RemoteImporter(ImportTarget target): importTarget(std::move(target)) {}

std::vector<compat::Diagnostic> RemoteImporter::takeDiagnostics(){
    std::vector<compat::Diagnostic> taken;
    taken.swap(reported);
    return taken;
}

void RemoteImporter::setProgress(std::function<void(const std::string&)> progress){
    progressLine = std::move(progress);
}

void RemoteImporter::progress(const std::string& line) const{
    if (progressLine)
        progressLine(line);
}

void RemoteImporter::report(compat::DiagCode code, const ImportLocation& at, const std::string& url, std::string message,
                            std::string suggestion){
    compat::Diagnostic diagnostic;
    diagnostic.code = code;
    const compat::DiagCodeInfo* info = compat::findDiagnosticCode(code);
    diagnostic.severity = info ? info->severity : compat::Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.suggestion = std::move(suggestion);
    diagnostic.subject = url;
    diagnostic.file = at.file;
    diagnostic.line = at.line;
    diagnostic.column = at.column;
    diagnostic.length = at.line ? (uint32_t)url.size() : 0;
    reported.push_back(std::move(diagnostic));
}

bool RemoteImporter::fetch(const std::string& url, const ImportLocation& at, HttpResponse& response, std::string& body,
                           std::string& error, bool quiet){
    body.clear();
    if (HttpClient::mirrorDirectory().empty() && !HttpClient::available()){
        // reported (once) even when the caller words download errors itself: nothing can be imported
        error = "the curl executable was not found";
        if (!curlMissingReported){
            curlMissingReported = true;
            report(compat::codes::CurlMissing, at, url, "cannot download '" + url + "': the curl executable was not found on PATH",
#ifdef _WIN32
                   "curl.exe ships with Windows 10 1803 and later in C:\\Windows\\System32; install it from https://curl.se/windows/ and add it to PATH"
#elif defined(__APPLE__)
                   "macOS ships curl in /usr/bin; restore it or install it with Homebrew (brew install curl)"
#else
                   "install curl with the system package manager (for example: sudo apt install curl)"
#endif
            );
        }
        return false;
    }
    response = client.get(url);
    if (!response.ok()){
        error = response.error.empty() ? "HTTP " + std::to_string(response.status) : response.error;
        if (!quiet)
            report(compat::codes::DownloadFailed, at, url, "download of '" + url + "' failed: " + error,
                   "check the URL and the network, then import again (Inspector: Import, or jasperengine-editor import-ui)");
        return false;
    }
    if (!HttpClient::readBody(response, body)){
        error = "the downloaded body could not be read";
        if (!quiet)
            report(compat::codes::DownloadFailed, at, url, "download of '" + url + "' failed: " + error);
        return false;
    }
    return true;
}

std::string RemoteImporter::importPath(const std::string& relative) const{
    return importTarget.root.empty() ? relative : importTarget.root + "/" + relative;
}

bool RemoteImporter::finish(ImportStaging& staging, std::vector<ImportEntry> entries, ImportedResource& result){
    std::string error;
    if (!staging.commit(importTarget, result.written, error)){
        result.ok = false;
        result.error = error;
        report(compat::codes::DownloadFailed, ImportLocation(), result.url, "import of '" + result.url + "' failed: " + error);
        return false;
    }
    const std::string fetched = ImportManifestWriter::nowIso8601();
    for (ImportEntry& entry : entries){
        entry.url = ImportManifest::normalizeUrl(entry.url);
        if (entry.fetched.empty())
            entry.fetched = fetched;
        if (entry.sha256.empty()){
            const auto it = staging.files().find(entry.path);
            std::string content;
            if (it != staging.files().end())
                entry.sha256 = compat::sha256Hex(it->second);
            else if (readFile(importTarget.toAbsolute(entry.path), content))
                entry.sha256 = compat::sha256Hex(content);
        }
    }
    ImportManifestWriter writer(importTarget.assetsDir);
    if (!writer.add(entries, &error)){
        result.ok = false;
        result.error = error;
        report(compat::codes::DownloadFailed, ImportLocation(), result.url, "the import manifest could not be written: " + error);
        return false;
    }
    result.entries.insert(result.entries.end(), entries.begin(), entries.end());
    result.ok = true;
    if (!entries.empty() && !staging.files().empty()){
        const ImportEntry& entry = entries.front();
        report(compat::codes::Imported, ImportLocation(), entry.url, "imported '" + result.url + "' as " + entry.path + " (" +
               std::to_string(staging.files().size()) + (staging.files().size() == 1 ? " file" : " files") + ", " + entry.kind + ")");
    }
    return true;
}

ImportedResource RemoteImporter::importNested(const std::string& url, const std::string& kindHint, const ImportLocation& at){
    const std::string key = ImportManifest::normalizeUrl(url);
    const auto it = nested.find(key);
    if (it != nested.end())
        return it->second;
    ImportedResource result = import(url, kindHint, at);
    nested[key] = result;
    return result;
}

ImportedResource RemoteImporter::import(const std::string& url, const std::string& kindHint, const ImportLocation& at){
    ImportedResource result;
    result.url = url;

    if (UrlResolver::classify(url) != UrlKind::Remote){
        result.error = "'" + url + "' is not an http(s) URL";
        report(compat::codes::DownloadFailed, at, url, "cannot import '" + url + "': not an http(s) URL");
        return result;
    }
    progress("[HTML UI] importing " + url);

    if (GoogleFontsImporter::isGoogleFontsUrl(url))
        return GoogleFontsImporter(*this).run(url, at);

    HttpResponse response;
    std::string body;
    if (!fetch(url, at, response, body, result.error))
        return result;
    const std::string finalUrl = response.finalUrl.empty() ? url : response.finalUrl;

    const SniffedContent sniffed = sniffContent(body, response.contentType, finalUrl);
    using Type = SniffedContent::Type;
    switch (sniffed.type){
        case Type::Css:
            return StylesheetImporter(*this).run(url, finalUrl, body, at);
        case Type::TrueType:
        case Type::OpenType:
        case Type::Woff:
        case Type::Woff2:
            return importFontFile(*this, url, body, sniffed, at);
        case Type::Zip:
            return LottieImporter(*this).runDotLottie(url, finalUrl, body, at);
        case Type::Json:
            if (LottieImporter::looksLikeLottie(body))
                return LottieImporter(*this).runJson(url, finalUrl, body, at);
            break;
        case Type::Html:
            if (LottieImporter::isLottieFilesPage(finalUrl))
                return LottieImporter(*this).runPage(url, finalUrl, body, at);
            break;
        case Type::Unknown:
            // a stylesheet served as text/plain or without a type
            if ((kindHint == "stylesheet" || urlExtension(finalUrl) == "css") && body.find('\0') == std::string::npos)
                return StylesheetImporter(*this).run(url, finalUrl, body, at);
            break;
        default:
            break;
    }
    if (sniffed.isImage())
        return ImageImporter(*this).run(url, finalUrl, body, sniffed, at);
    // phase 6: copied when the engine plays it, else converted by the editor's FFmpeg
    if (sniffed.isVideo())
        return RemoteVideoImporter(*this).run(url, body, sniffed, at);

    const std::string what = response.contentType.empty() ? std::string(sniffed.name()) : response.contentType;
    result.error = "unsupported content (" + what + ")";
    report(compat::codes::UnsupportedContent, at, url,
           "'" + url + "' returned " + what + ", which is not an image, SVG, font, stylesheet, Lottie animation or video; nothing imported",
           "link the file itself (an image, .ttf, .css, .json, .lottie or video URL)");
    return result;
}
