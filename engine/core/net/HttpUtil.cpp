// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "net/HttpBackend.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace jasperengine;

namespace {

    bool isUnreserved(unsigned char c){
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '.' || c == '_' || c == '~';
    }

    int hexValue(char c){
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // RFC 3986 5.2.4
    std::string removeDotSegments(const std::string& path){
        std::string input = path;
        std::string output;
        while (!input.empty()){
            if (input.compare(0, 3, "../") == 0){
                input.erase(0, 3);
            } else if (input.compare(0, 2, "./") == 0){
                input.erase(0, 2);
            } else if (input.compare(0, 3, "/./") == 0){
                input.erase(0, 2);
            } else if (input == "/."){
                input = "/";
            } else if (input.compare(0, 4, "/../") == 0 || input == "/.."){
                input = input.size() == 3 ? "/" : input.substr(3);
                const size_t slash = output.rfind('/');
                output.erase(slash == std::string::npos ? 0 : slash);
            } else if (input == "." || input == ".."){
                input.clear();
            } else {
                size_t next = input.find('/', input[0] == '/' ? 1 : 0);
                if (next == std::string::npos) next = input.size();
                output += input.substr(0, next);
                input.erase(0, next);
            }
        }
        return output;
    }

    bool hasScheme(const std::string& reference){
        if (reference.empty() || !std::isalpha((unsigned char)reference[0])) return false;
        for (size_t i = 1; i < reference.size(); i++){
            const unsigned char c = (unsigned char)reference[i];
            if (c == ':') return true;
            if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') return false;
        }
        return false;
    }

    std::string trim(const std::string& text){
        size_t start = 0, end = text.size();
        while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n')) start++;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) end--;
        return text.substr(start, end - start);
    }

}

bool HttpUtil::parseUrl(const std::string& url, Url& out){
    out = Url();
    for (unsigned char c : url){
        if (c <= 0x20 || c == 0x7f) return false;
        if (c == '<' || c == '>' || c == '"' || c == '\\' || c == '^' || c == '`' || c == '{' || c == '|' || c == '}') return false;
    }
    const size_t sep = url.find("://");
    if (sep == std::string::npos || sep == 0) return false;
    out.scheme = lower(url.substr(0, sep));
    if (out.scheme != "http" && out.scheme != "https") return false;
    const size_t authorityStart = sep + 3;
    size_t authorityEnd = url.find_first_of("/?#", authorityStart);
    if (authorityEnd == std::string::npos) authorityEnd = url.size();
    std::string authority = url.substr(authorityStart, authorityEnd - authorityStart);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    if (authority.empty()) return false;

    std::string portText;
    if (authority[0] == '['){
        const size_t close = authority.find(']');
        if (close == std::string::npos || close == 1) return false;
        out.host = authority.substr(1, close - 1);
        const std::string rest = authority.substr(close + 1);
        if (!rest.empty()){
            if (rest[0] != ':') return false;
            portText = rest.substr(1);
        }
        for (char c : out.host){
            if (!std::isxdigit((unsigned char)c) && c != ':' && c != '.' && c != '%') return false;
        }
    } else {
        const size_t colon = authority.find(':');
        out.host = authority.substr(0, colon);
        if (colon != std::string::npos) portText = authority.substr(colon + 1);
        if (out.host.empty() || out.host.find('[') != std::string::npos || out.host.find(']') != std::string::npos) return false;
    }
    if (out.host.empty()) return false;
    out.port = out.scheme == "https" ? 443 : 80;
    if (!portText.empty()){
        if (portText.size() > 5) return false;
        for (char c : portText){
            if (!std::isdigit((unsigned char)c)) return false;
        }
        const int port = std::atoi(portText.c_str());
        if (port < 1 || port > 65535) return false;
        out.port = port;
        out.explicitPort = true;
    }
    std::string target = url.substr(authorityEnd);
    const size_t hash = target.find('#');
    if (hash != std::string::npos) target.erase(hash);
    if (target.empty() || target[0] == '?') target = "/" + target;
    out.target = target;
    return true;
}

bool HttpUtil::isLocalHost(const std::string& host){
    const std::string h = lower(host);
    return h == "localhost" || h == "127.0.0.1" || h == "::1" || h == "[::1]";
}

std::string HttpUtil::checkPolicy(const std::string& url, bool allowInsecure, std::string& error){
    Url parsed;
    if (!parseUrl(url, parsed)){
        if (url.find(' ') != std::string::npos){
            error = "invalid URL (a space must be percent-encoded as %20, see Http.encodeUrl): " + url;
        } else if (url.find("://") == std::string::npos || (lower(url).compare(0, 7, "http://") != 0 && lower(url).compare(0, 8, "https://") != 0)){
            error = "invalid URL (only http:// and https:// URLs are supported): " + url;
        } else {
            error = "invalid URL: " + url;
        }
        return "invalid_url";
    }
    if (parsed.scheme == "http" && !allowInsecure && !isLocalHost(parsed.host)){
        error = "plain http:// is not allowed (use https://, or turn on Project Settings > Player > Allow Insecure HTTP, "
                "player.allow_insecure_http): " + url;
        return "insecure_http";
    }
    return "";
}

std::string HttpUtil::stripFragment(const std::string& url){
    const size_t hash = url.find('#');
    return hash == std::string::npos ? url : url.substr(0, hash);
}

std::string HttpUtil::resolveUrl(const std::string& base, const std::string& reference){
    const std::string ref = trim(reference);
    if (hasScheme(ref)) return ref;
    const size_t sep = base.find("://");
    if (sep == std::string::npos) return ref;
    const std::string scheme = base.substr(0, sep);
    size_t authorityEnd = base.find_first_of("/?#", sep + 3);
    if (authorityEnd == std::string::npos) authorityEnd = base.size();
    const std::string origin = base.substr(0, authorityEnd);
    std::string rest = stripFragment(base.substr(authorityEnd));
    const size_t q = rest.find('?');
    std::string basePath = q == std::string::npos ? rest : rest.substr(0, q);
    const std::string baseQuery = q == std::string::npos ? "" : rest.substr(q);
    if (basePath.empty()) basePath = "/";

    if (ref.empty()) return origin + basePath + baseQuery;
    if (ref.compare(0, 2, "//") == 0) return scheme + ":" + ref;
    if (ref[0] == '#') return origin + basePath + baseQuery + ref;
    if (ref[0] == '?') return origin + basePath + ref;

    std::string refPath = ref;
    std::string refTail;
    const size_t tail = ref.find_first_of("?#");
    if (tail != std::string::npos){
        refPath = ref.substr(0, tail);
        refTail = ref.substr(tail);
    }
    std::string merged;
    if (!refPath.empty() && refPath[0] == '/'){
        merged = refPath;
    } else {
        const size_t slash = basePath.rfind('/');
        merged = (slash == std::string::npos ? std::string("/") : basePath.substr(0, slash + 1)) + refPath;
    }
    merged = removeDotSegments(merged);
    if (merged.empty() || merged[0] != '/') merged = "/" + merged;
    return origin + merged + refTail;
}

std::string HttpUtil::appendQuery(const std::string& url, const HttpHeaders& query){
    if (query.empty()) return url;
    std::string head = url;
    std::string fragment;
    const size_t hash = url.find('#');
    if (hash != std::string::npos){
        head = url.substr(0, hash);
        fragment = url.substr(hash);
    }
    std::string text;
    for (const auto& [name, value] : query){
        if (!text.empty()) text += '&';
        text += encode(name);
        text += '=';
        text += encode(value);
    }
    if (head.find('?') == std::string::npos){
        head += '?';
    } else if (head.back() != '?' && head.back() != '&'){
        head += '&';
    }
    return head + text + fragment;
}

std::string HttpUtil::encode(const std::string& text){
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char c : text){
        if (isUnreserved(c)){
            out += (char)c;
        } else {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
        }
    }
    return out;
}

std::string HttpUtil::decode(const std::string& text, bool plusAsSpace){
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++){
        const char c = text[i];
        if (c == '%' && i + 2 < text.size()){
            const int hi = hexValue(text[i + 1]);
            const int lo = hexValue(text[i + 2]);
            if (hi >= 0 && lo >= 0){
                out += (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += (plusAsSpace && c == '+') ? ' ' : c;
    }
    return out;
}

std::string HttpUtil::lower(std::string text){
    for (char& c : text){
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return text;
}

const char* HttpUtil::reasonPhrase(int status){
    switch (status){
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 203: return "Non-Authoritative Information";
        case 204: return "No Content";
        case 205: return "Reset Content";
        case 206: return "Partial Content";
        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 402: return "Payment Required";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 407: return "Proxy Authentication Required";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 417: return "Expectation Failed";
        case 418: return "I'm a teapot";
        case 421: return "Misdirected Request";
        case 422: return "Unprocessable Content";
        case 425: return "Too Early";
        case 426: return "Upgrade Required";
        case 428: return "Precondition Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 451: return "Unavailable For Legal Reasons";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        case 511: return "Network Authentication Required";
        default: return "";
    }
}

bool HttpUtil::isToken(const std::string& text){
    if (text.empty()) return false;
    for (unsigned char c : text){
        if (std::isalnum(c)) continue;
        switch (c){
            case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
            case '-': case '.': case '^': case '_': case '`': case '|': case '~':
                continue;
            default:
                return false;
        }
    }
    return true;
}

std::string HttpUtil::seconds(double value){
    char text[64];
    if (std::fabs(value - std::round(value)) < 1e-9){
        std::snprintf(text, sizeof(text), "%.0f", value);
        return text;
    }
    std::snprintf(text, sizeof(text), "%.3f", value);
    std::string out = text;
    while (!out.empty() && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

HttpHeaders HttpUtil::parseHeaderBlock(const std::string& raw, int* status){
    HttpHeaders headers;
    size_t start = 0;
    while (start < raw.size()){
        size_t end = raw.find('\n', start);
        if (end == std::string::npos) end = raw.size();
        std::string line = raw.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.compare(0, 5, "HTTP/") == 0){
            // a new response (an interim 1xx, or the next hop): its headers replace the last ones
            headers.clear();
            if (status){
                const size_t space = line.find(' ');
                *status = space == std::string::npos ? 0 : std::atoi(line.c_str() + space + 1);
            }
            continue;
        }
        if ((line[0] == ' ' || line[0] == '\t') && !headers.empty()){
            headers.back().second += " " + trim(line);
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        headers.emplace_back(lower(trim(line.substr(0, colon))), trim(line.substr(colon + 1)));
    }
    return headers;
}

namespace {

    bool parseIpv4(const std::string& host, unsigned char out[4]){
        int part = 0;
        size_t i = 0;
        while (part < 4){
            if (i >= host.size() || !std::isdigit((unsigned char)host[i])) return false;
            int value = 0;
            size_t digits = 0;
            while (i < host.size() && std::isdigit((unsigned char)host[i])){
                value = value * 10 + (host[i] - '0');
                if (++digits > 3 || value > 255) return false;
                i++;
            }
            out[part++] = (unsigned char)value;
            if (part < 4){
                if (i >= host.size() || host[i] != '.') return false;
                i++;
            }
        }
        return i == host.size();
    }

    bool isPrivateIpv4(const unsigned char a[4]){
        return a[0] == 127 || a[0] == 10 || a[0] == 0 ||
               (a[0] == 172 && (a[1] & 0xF0) == 16) ||
               (a[0] == 192 && a[1] == 168) ||
               (a[0] == 169 && a[1] == 254) ||
               (a[0] == 100 && (a[1] & 0xC0) == 64);
    }

}

bool HttpUtil::isPrivateHost(const std::string& rawHost){
    std::string host = lower(rawHost);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    while (!host.empty() && host.back() == '.') host.pop_back();
    if (host.empty()) return false;
    if (host == "localhost" || (host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0)) return true;
    unsigned char v4[4];
    if (parseIpv4(host, v4)) return isPrivateIpv4(v4);
    if (host.find(':') == std::string::npos) return false;
    // IPv6 literal (a zone "%eth0" is link-local)
    if (host.find('%') != std::string::npos) return true;
    if (host == "::1" || host == "::") return true;
    // ::ffff:a.b.c.d
    const size_t lastColon = host.rfind(':');
    if (host.compare(0, 7, "::ffff:") == 0 && parseIpv4(host.substr(lastColon + 1), v4)) return isPrivateIpv4(v4);
    // the first 16-bit group
    const size_t colon = host.find(':');
    const std::string first = host.substr(0, colon);
    if (first.empty() || first.size() > 4) return false;
    unsigned value = 0;
    for (char c : first){
        if (!std::isxdigit((unsigned char)c)) return false;
        value = value * 16 + (unsigned)(std::isdigit((unsigned char)c) ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return (value & 0xFE00) == 0xFC00 || (value & 0xFFC0) == 0xFE80;
}
