// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ContentSniffer.h"

#include "htmlui/compat/UrlResolver.h"

#include <cstring>

using namespace jasperengine::editor::htmlui;

namespace {

    bool startsWith(const std::string& body, const char* magic, size_t length, size_t at = 0){
        return body.size() >= at + length && std::memcmp(body.data() + at, magic, length) == 0;
    }

    std::string lower(std::string text){
        for (char& c : text){
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
        }
        return text;
    }

    // the first markup of a text body after a BOM, whitespace, the XML declaration, comments
    // and a doctype ("svg", "html", "!doctype html" -> "html"); "" when it does not start with '<'
    std::string firstTag(const std::string& body){
        size_t i = 0;
        if (startsWith(body, "\xEF\xBB\xBF", 3))
            i = 3;
        const size_t limit = std::min(body.size(), (size_t)8192);
        while (i < limit){
            while (i < limit && (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n'))
                i++;
            if (i >= limit || body[i] != '<')
                return "";
            if (body.compare(i, 4, "<!--") == 0){
                const size_t end = body.find("-->", i + 4);
                if (end == std::string::npos)
                    return "";
                i = end + 3;
                continue;
            }
            if (body.compare(i, 2, "<?") == 0){
                const size_t end = body.find("?>", i + 2);
                if (end == std::string::npos)
                    return "";
                i = end + 2;
                continue;
            }
            size_t end = i + 1;
            while (end < body.size() && body[end] != ' ' && body[end] != '>' && body[end] != '\t' && body[end] != '\r' &&
                   body[end] != '\n' && body[end] != '/')
                end++;
            std::string name = lower(body.substr(i + 1, end - i - 1));
            if (name == "!doctype"){
                const size_t close = body.find('>', end);
                const std::string doctype = lower(body.substr(end, close == std::string::npos ? std::string::npos : close - end));
                if (doctype.find("svg") != std::string::npos)
                    return "svg";
                if (doctype.find("html") != std::string::npos)
                    return "html";
                if (close == std::string::npos)
                    return "";
                i = close + 1;
                continue;
            }
            const size_t colon = name.find(':');
            if (colon != std::string::npos)
                name.erase(0, colon + 1);   // svg:svg
            return name;
        }
        return "";
    }

    bool looksLikeJson(const std::string& body){
        for (char c : body){
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                continue;
            return c == '{' || c == '[';
        }
        return false;
    }

    bool looksBinary(const std::string& body){
        const size_t limit = std::min(body.size(), (size_t)1024);
        for (size_t i = 0; i < limit; i++){
            if (body[i] == '\0')
                return true;
        }
        return false;
    }

}

bool SniffedContent::isImage() const{
    switch (type){
        case Type::Png: case Type::Jpeg: case Type::Gif: case Type::Bmp: case Type::Tga: case Type::WebP: case Type::Avif: case Type::Svg:
            return true;
        default:
            return false;
    }
}

bool SniffedContent::isDecodableImage() const{
    return isImage() && type != Type::WebP && type != Type::Avif;
}

bool SniffedContent::isFont() const{
    return type == Type::TrueType || type == Type::OpenType || type == Type::Woff || type == Type::Woff2;
}

bool SniffedContent::isVideo() const{
    return type == Type::WebM || type == Type::Mp4 || type == Type::QuickTime || type == Type::Ogg;
}

std::string SniffedContent::extension() const{
    switch (type){
        case Type::Png: return "png";
        case Type::Jpeg: return "jpg";
        case Type::Gif: return "gif";
        case Type::Bmp: return "bmp";
        case Type::Tga: return "tga";
        case Type::WebP: return "webp";
        case Type::Avif: return "avif";
        case Type::Svg: return "svg";
        case Type::TrueType: return "ttf";
        case Type::OpenType: return "otf";
        case Type::Woff: return "woff";
        case Type::Woff2: return "woff2";
        case Type::Zip: return "lottie";
        case Type::Json: return "json";
        case Type::Css: return "css";
        case Type::Html: return "html";
        case Type::WebM: return "webm";
        case Type::Mp4: return "mp4";
        case Type::QuickTime: return "mov";
        case Type::Ogg: return "ogv";
        case Type::Unknown: break;
    }
    return "";
}

const char* SniffedContent::name() const{
    switch (type){
        case Type::Png: return "PNG";
        case Type::Jpeg: return "JPEG";
        case Type::Gif: return "GIF";
        case Type::Bmp: return "BMP";
        case Type::Tga: return "TGA";
        case Type::WebP: return "WebP";
        case Type::Avif: return "AVIF";
        case Type::Svg: return "SVG";
        case Type::TrueType: return "TrueType font";
        case Type::OpenType: return "OpenType font";
        case Type::Woff: return "WOFF font";
        case Type::Woff2: return "WOFF2 font";
        case Type::Zip: return "ZIP archive";
        case Type::Json: return "JSON";
        case Type::Css: return "CSS";
        case Type::Html: return "HTML page";
        case Type::WebM: return "WebM/Matroska video";
        case Type::Mp4: return "MP4 video";
        case Type::QuickTime: return "QuickTime video";
        case Type::Ogg: return "Ogg video";
        case Type::Unknown: break;
    }
    return "unknown content";
}

std::string jasperengine::editor::htmlui::urlExtension(const std::string& url){
    std::string path = url;
    const size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos)
        path.erase(cut);
    const size_t scheme = path.find("://");
    const size_t pathStart = path.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    if (pathStart == std::string::npos)
        return "";
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot < slash || dot + 1 >= path.size())
        return "";
    return lower(path.substr(dot + 1));
}

std::string jasperengine::editor::htmlui::urlBaseName(const std::string& url){
    std::string path = url;
    const size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos)
        path.erase(cut);
    const size_t scheme = path.find("://");
    const size_t pathStart = path.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    if (pathStart == std::string::npos)
        return "";
    std::string name = path.substr(path.find_last_of('/') + 1);
    name = jasperengine::htmlui::compat::UrlResolver::percentDecode(name);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        name.erase(dot);
    return name;
}

SniffedContent jasperengine::editor::htmlui::sniffContent(const std::string& body, const std::string& contentType, const std::string& url){
    using Type = SniffedContent::Type;
    SniffedContent result;
    auto is = [&result](Type type){ result.type = type; return result; };

    // magic bytes
    if (startsWith(body, "\x89PNG\r\n\x1A\n", 8)) return is(Type::Png);
    if (startsWith(body, "\xFF\xD8\xFF", 3)) return is(Type::Jpeg);
    if (startsWith(body, "GIF87a", 6) || startsWith(body, "GIF89a", 6)) return is(Type::Gif);
    if (startsWith(body, "RIFF", 4) && startsWith(body, "WEBP", 4, 8)) return is(Type::WebP);
    if (startsWith(body, "ftyp", 4, 4) && (startsWith(body, "avif", 4, 8) || startsWith(body, "avis", 4, 8))) return is(Type::Avif);
    if (startsWith(body, "wOFF", 4)) return is(Type::Woff);
    if (startsWith(body, "wOF2", 4)) return is(Type::Woff2);
    if (startsWith(body, "\x00\x01\x00\x00", 4) || startsWith(body, "true", 4) || startsWith(body, "ttcf", 4)) return is(Type::TrueType);
    if (startsWith(body, "OTTO", 4)) return is(Type::OpenType);
    if (startsWith(body, "PK\x03\x04", 4)) return is(Type::Zip);
    // videos (phase 6): EBML, an ISO base media "ftyp" box (AVIF above is one too), Ogg
    if (startsWith(body, "\x1A\x45\xDF\xA3", 4)) return is(Type::WebM);
    if (startsWith(body, "ftyp", 4, 4)) return is(startsWith(body, "qt  ", 4, 8) ? Type::QuickTime : Type::Mp4);
    if (startsWith(body, "OggS", 4)) return is(Type::Ogg);
    if (startsWith(body, "BM", 2) && body.size() >= 26 && (contentType == "image/bmp" || contentType == "image/x-ms-bmp" || urlExtension(url) == "bmp"))
        return is(Type::Bmp);

    const std::string extension = urlExtension(url);
    // TGA has no magic: trust the server or the URL
    if (contentType == "image/x-tga" || contentType == "image/tga" || contentType == "image/x-targa" || extension == "tga")
        return is(Type::Tga);

    if (looksBinary(body))
        return result;

    const std::string tag = firstTag(body);
    if (tag == "svg") return is(Type::Svg);
    if (tag == "html" || tag == "head" || tag == "body") return is(Type::Html);
    if (contentType == "image/svg+xml" && body.find("<svg") != std::string::npos) return is(Type::Svg);

    if (looksLikeJson(body) && (contentType.empty() || contentType == "application/json" || contentType == "text/json" ||
                                contentType == "text/plain" || contentType == "application/octet-stream" ||
                                contentType.find("json") != std::string::npos || extension == "json"))
        return is(Type::Json);
    if (contentType == "text/css" || extension == "css") return is(Type::Css);
    if (contentType == "text/html") return is(Type::Html);
    return result;
}
