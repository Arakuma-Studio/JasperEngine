// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UrlResolver.h"
#include "htmlui/compat/LiteralCompare.h"

#include "Diagnostics.h"
#include "Hash.h"
#include "ImportManifest.h"
#include "VirtualFiles.h"

#include <vector>

using namespace jasperengine::htmlui::compat;

namespace {

    std::string trim(std::string_view text){
        size_t begin = 0, end = text.size();
        // HTML strips ASCII whitespace around URLs
        while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' || text[begin] == '\r' || text[begin] == '\f'))
            begin++;
        while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' || text[end - 1] == '\r' || text[end - 1] == '\f'))
            end--;
        return std::string(text.substr(begin, end - begin));
    }

    std::string lower(std::string_view text){
        std::string result(text);
        for (char& c : result){
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
        }
        return result;
    }

    bool startsWithNoCase(std::string_view text, std::string_view prefix){
        if (text.size() < prefix.size())
            return false;
        return lower(text.substr(0, prefix.size())) == prefix;
    }

    // "scheme:" at the start (RFC 3986: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":")
    std::string schemeOf(std::string_view url){
        if (url.empty() || !((url[0] >= 'a' && url[0] <= 'z') || (url[0] >= 'A' && url[0] <= 'Z')))
            return std::string();
        for (size_t i = 1; i < url.size(); i++){
            const char c = url[i];
            if (c == ':')
                return lower(url.substr(0, i));
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'))
                return std::string();
        }
        return std::string();
    }

    int base64Value(char c){
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    }

    // lenient: whitespace skipped, stops at '=' or an invalid character
    std::string decodeBase64(std::string_view text){
        std::string out;
        out.reserve(text.size() * 3 / 4);
        uint32_t buffer = 0;
        int bits = 0;
        for (char c : text){
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
                continue;
            if (c == '=')
                break;
            const int value = base64Value(c);
            if (value < 0)
                break;
            buffer = (buffer << 6) | (uint32_t)value;
            bits += 6;
            if (bits >= 8){
                bits -= 8;
                out += (char)((buffer >> bits) & 0xFF);
            }
        }
        return out;
    }

    // "icons.svg?v=2#home" -> path "icons.svg", fragment "#home"
    void splitQueryAndFragment(std::string& path, std::string& fragment){
        const size_t hash = path.find('#');
        if (hash != std::string::npos){
            fragment = path.substr(hash);
            path.erase(hash);
        }
        const size_t query = path.find('?');
        if (query != std::string::npos)
            path.erase(query);
    }

    std::string describe(std::string_view resourceKind){
        return resourceKind.empty() ? std::string("resource") : std::string(resourceKind);
    }

    // what happens to a remote resource that was not imported (phase-2 §3.13.6)
    std::string fallbackOf(std::string_view kind){
        if (kind == "stylesheet" || kind == "google-fonts-css")
            return "the stylesheet is skipped; fonts fall back through the font chain";
        if (kind == "font")
            return "the face is not registered; the text uses the fallback chain";
        if (kind == "image")
            return "the image is not drawn; the element keeps its CSS size";
        if (kind == "lottie")
            return "the animation stays empty";
        if (kind == "svg")
            return "the element stays empty";
        return "it is not loaded";
    }

}

UrlResolver::UrlResolver(const FileAccess& files, const ImportManifest* manifest): files(files), manifestOverride(manifest) {
}

const ImportManifest& UrlResolver::manifest() const{
    return manifestOverride ? *manifestOverride : ImportManifest::get();
}

UrlKind UrlResolver::classify(std::string_view input){
    const std::string url = trim(input);
    if (url.empty())
        return UrlKind::Empty;
    if (url[0] == '#')
        return UrlKind::Fragment;
    if (url.compare(0, 2, "//") == 0)
        return UrlKind::Remote;
    if (VirtualFiles::isVirtual(url))
        return UrlKind::Virtual;
    if (url[0] == '/' || url[0] == '\\')
        return UrlKind::RootRelative;

    const std::string scheme = schemeOf(url);
    // "C:/..." is a drive letter, not a scheme: an absolute path, kept relative here
    if (scheme.size() == 1)
        return UrlKind::Relative;
    if (scheme.empty())
        return UrlKind::Relative;
    if (scheme == "http" || scheme == "https")
        return UrlKind::Remote;
    if (scheme == "data"){
        // the engine's user data scheme is "data://"; a data: URI never has "//"
        if (url.compare(0, 7, "data://") == 0)
            return UrlKind::EnginePath;
        return UrlKind::Data;
    }
    if (scheme == "asset")
        return UrlKind::RootRelative;
    if (scheme == "lua")
        return UrlKind::EnginePath;
    return UrlKind::OtherScheme;
}

std::string UrlResolver::percentDecode(std::string_view text){
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++){
        if (text[i] == '%' && i + 2 < text.size()){
            const int high = hex(text[i + 1]);
            const int low = hex(text[i + 2]);
            if (high >= 0 && low >= 0){
                out += (char)(high * 16 + low);
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

std::string UrlResolver::normalizePath(std::string_view input, bool* outside){
    if (outside)
        *outside = false;
    std::string path(input);
    for (char& c : path){
        if (c == '\\')
            c = '/';
    }
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()){
        size_t end = path.find('/', start);
        if (end == std::string::npos)
            end = path.size();
        const std::string part = path.substr(start, end - start);
        if (part == ".."){
            if (parts.empty()){
                if (outside)
                    *outside = true;
            }else{
                parts.pop_back();
            }
        }else if (!part.empty() && part != "."){
            parts.push_back(part);
        }
        start = end + 1;
    }
    std::string result;
    for (size_t i = 0; i < parts.size(); i++){
        if (i > 0)
            result += '/';
        result += parts[i];
    }
    return result;
}

std::string UrlResolver::joinPath(const std::string& fromFile, std::string_view relative, bool* outside){
    std::string base = fromFile;
    for (char& c : base){
        if (c == '\\')
            c = '/';
    }
    if (base.compare(0, 8, "asset://") == 0)
        base.erase(0, 8);
    const size_t slash = base.rfind('/');
    const std::string directory = slash == std::string::npos ? std::string() : base.substr(0, slash + 1);
    // a document opened by an absolute filesystem path (tests, tools) keeps its references
    // absolute, as phase 1's RmlSystem::joinPath does
    if (!base.empty() && base[0] == '/')
        return "/" + normalizePath(directory + std::string(relative), outside);
    return normalizePath(directory + std::string(relative), outside);
}

std::optional<ParsedDataUri> UrlResolver::parseDataUri(std::string_view input){
    const std::string url = trim(input);
    if (!startsWithNoCase(url, "data:"))
        return std::nullopt;
    const size_t comma = url.find(',');
    if (comma == std::string::npos)
        return std::nullopt;

    ParsedDataUri result;
    std::string header = lower(url.substr(5, comma - 5));
    if (header.size() >= 7 && header.compare(header.size() - 7, 7, ";base64") == 0){
        result.base64 = true;
        header.erase(header.size() - 7);
    }
    const size_t parameters = header.find(';');
    result.mimeType = trim(parameters == std::string::npos ? header : header.substr(0, parameters));
    if (result.mimeType.empty())
        result.mimeType = "text/plain";

    const std::string_view payload = std::string_view(url).substr(comma + 1);
    result.data = result.base64 ? decodeBase64(percentDecode(payload)) : percentDecode(payload);
    return result;
}

std::string UrlResolver::extensionForMime(std::string_view mimeType){
    const std::string mime = lower(mimeType);
    if (mime == "image/png") return "png";
    if (mime == "image/jpeg" || mime == "image/jpg" || mime == "image/pjpeg") return "jpg";
    if (mime == "image/gif") return "gif";
    if (mime == "image/bmp" || mime == "image/x-ms-bmp") return "bmp";
    if (mime == "image/x-tga" || mime == "image/tga" || mime == "image/x-targa") return "tga";
    if (mime == "image/webp") return "webp";
    if (mime == "image/avif") return "avif";
    if (mime == "image/svg+xml") return "svg";
    if (mime == "application/json" || mime == "text/json" || mime == "application/lottie+json") return "json";
    if (mime == "font/ttf" || mime == "font/sfnt" || mime == "application/x-font-ttf" || mime == "application/font-sfnt" ||
        mime == "application/x-font-truetype" || mime == "font/truetype") return "ttf";
    if (mime == "font/otf" || mime == "application/x-font-otf" || mime == "application/x-font-opentype" || mime == "font/opentype") return "otf";
    if (mime == "text/css") return "css";
    if (mime == "text/plain") return "txt";
    return "bin";
}

std::string UrlResolver::dataPath(const std::string& data, std::string_view mimeType){
    return std::string(VirtualFiles::kPrefix) + "data/" + sha1Hex(data).substr(0, 12) + "." + extensionForMime(mimeType);
}

ResolvedUrl UrlResolver::resolve(std::string_view input, const std::string& fromFile, std::string_view resourceKind, bool checkExists) const{
    ResolvedUrl result;
    result.url = trim(input);
    result.kind = classify(result.url);

    switch (result.kind){
        case UrlKind::Empty:
            result.exists = false;
            break;

        case UrlKind::Fragment:
            result.fragment = result.url;
            result.emitted = result.url;
            break;

        case UrlKind::OtherScheme:
            result.exists = false;
            break;

        case UrlKind::Virtual: {
            std::string path = result.url;
            splitQueryAndFragment(path, result.fragment);
            result.assetPath = VirtualFiles::normalize(path);
            result.emitted = "/" + result.assetPath + result.fragment;
            if (checkExists)
                result.exists = VirtualFiles::exists(result.assetPath);
            break;
        }

        case UrlKind::Data: {
            std::optional<ParsedDataUri> parsed = parseDataUri(result.url);
            if (!parsed){
                result.exists = false;
                break;
            }
            result.mimeType = parsed->mimeType;
            result.assetPath = dataPath(parsed->data, parsed->mimeType);
            result.emitted = "/" + result.assetPath;
            result.data = std::make_shared<const std::string>(std::move(parsed->data));
            break;
        }

        case UrlKind::Remote: {
            const ImportEntry* entry = manifest().find(result.url);
            result.entry = entry;
            if (!entry){
                result.exists = false;
                break;
            }
            result.assetPath = normalizePath(entry->path);
            std::string withoutQuery = result.url;
            splitQueryAndFragment(withoutQuery, result.fragment);
            result.emitted = std::string(kAssetScheme) + result.assetPath + result.fragment;
            if (checkExists)
                result.exists = assetFileExists(files, result.assetPath);
            break;
        }

        case UrlKind::EnginePath: {
            std::string path = result.url;
            splitQueryAndFragment(path, result.fragment);
            result.assetPath = path;
            result.emitted = path + result.fragment;
            if (checkExists)
                result.exists = assetFileExists(files, result.assetPath);
            break;
        }

        case UrlKind::RootRelative:
        case UrlKind::Relative: {
            std::string path = result.url;
            splitQueryAndFragment(path, result.fragment);
            path = percentDecode(path);
            if (result.kind == UrlKind::RootRelative){
                if (path.compare(0, 8, "asset://") == 0 || path.compare(0, 8, "ASSET://") == 0)
                    path.erase(0, 8);
                result.assetPath = normalizePath(path, &result.outsideAssets);
            }else{
                result.assetPath = joinPath(fromFile, path, &result.outsideAssets);
            }
            // an absolute filesystem path (a document opened by one) and compat's own virtual
            // files (a document compiled from them) are written as such
            if (!result.assetPath.empty() && result.assetPath[0] == '/')
                result.emitted = result.assetPath + result.fragment;
            else if (VirtualFiles::isVirtual(result.assetPath))
                result.emitted = "/" + result.assetPath + result.fragment;
            else
                result.emitted = std::string(kAssetScheme) + result.assetPath + result.fragment;
            if (checkExists)
                result.exists = !result.outsideAssets && assetFileExists(files, result.assetPath);
            break;
        }
    }

    (void)resourceKind;
    return result;
}

ResolvedUrl UrlResolver::resolve(std::string_view url, const std::string& fromFile, std::string_view resourceKind,
                                 SourceRef ref, DiagnosticSink* sink) const{
    ResolvedUrl result = resolve(url, fromFile, resourceKind, true);
    if (!sink)
        return result;

    switch (result.kind){
        case UrlKind::Relative:
        case UrlKind::RootRelative:
        case UrlKind::EnginePath:
        case UrlKind::Virtual:
            if (!result.exists){
                std::string message = describe(resourceKind) + " '" + result.url + "' not found";
                if (result.outsideAssets)
                    message += " (the path climbs above the assets directory)";
                else if (result.assetPath != result.url)
                    message += " (resolved to " + result.assetPath + ")";
                sink->report(codes::FileNotFound, ref, message,
                             "check the path: relative paths start at the referencing file's directory, '/' at the assets directory",
                             {}, result.url);
            }
            break;
        case UrlKind::Remote:
            if (!result.entry){
                sink->report(codes::NotImported, ref,
                             "remote " + describe(resourceKind) + " '" + result.url + "' is not imported; " + fallbackOf(resourceKind),
                             "import it: load the document in the editor (it imports remote resources), or run jasperengine-editor import-ui", {}, result.url);
            }else if (!result.exists){
                sink->report(codes::ManifestFileMissing, ref,
                             "the import of '" + result.url + "' points to '" + result.assetPath + "', which does not exist",
                             "re-import it: load the document in the editor (it imports remote resources), or run jasperengine-editor import-ui", {}, result.url);
            }
            break;
        case UrlKind::OtherScheme:
            sink->report(codes::LinkNotNavigable, ref,
                         "'" + result.url + "' is not a file the engine can load; dropped", {}, {}, result.url);
            break;
        case UrlKind::Data:
            if (!result.data)
                sink->report(codes::FileNotFound, ref, "invalid data: URI", "write data:<mime>[;base64],<data>", {}, "data:");
            break;
        case UrlKind::Empty:
        case UrlKind::Fragment:
            break;
    }
    return result;
}
