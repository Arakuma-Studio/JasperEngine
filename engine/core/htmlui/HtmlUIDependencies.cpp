// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUIDependencies.h"

#include "io/Data.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

using namespace jasperengine;

namespace {

    bool isIdentChar(char c){
        return std::isalnum((unsigned char)c) || c == '-' || c == '_';
    }

    bool isSpace(char c){
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    }

    std::string toLower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

    bool startsWithNoCase(const std::string& text, size_t pos, const char* prefix){
        for (size_t i = 0; prefix[i]; i++){
            if (pos + i >= text.size() || std::tolower((unsigned char)text[pos + i]) != std::tolower((unsigned char)prefix[i]))
                return false;
        }
        return true;
    }

    size_t findNoCase(const std::string& text, const char* needle, size_t from, size_t end){
        for (size_t i = from; i < end; i++){
            if (startsWithNoCase(text, i, needle))
                return i;
        }
        return std::string::npos;
    }

    std::string trim(const std::string& text){
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && isSpace(text[begin])) begin++;
        while (end > begin && isSpace(text[end - 1])) end--;
        return text.substr(begin, end - begin);
    }

    // byte offset -> 1-based line and column
    class LineMap {
    public:
        explicit LineMap(const std::string& text){
            starts.push_back(0);
            for (size_t i = 0; i < text.size(); i++){
                if (text[i] == '\n')
                    starts.push_back(i + 1);
            }
        }

        void locate(size_t offset, int& line, int& column) const{
            auto it = std::upper_bound(starts.begin(), starts.end(), offset);
            const size_t index = (size_t)(it - starts.begin()) - 1;
            line = (int)index + 1;
            column = (int)(offset - starts[index]) + 1;
        }

    private:
        std::vector<size_t> starts;
    };

    bool isRemote(const std::string& path){
        const std::string lower = toLower(path);
        return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0 || lower.rfind("//", 0) == 0;
    }

    bool isAbsolutePath(const std::string& path){
        if (!path.empty() && (path[0] == '/' || path[0] == '\\'))
            return true;
        return path.size() >= 2 && path[1] == ':' && std::isalpha((unsigned char)path[0]);
    }

    // not a file: inline data, a fragment, user data or Lua paths, a sprite name
    bool isSkipped(const std::string& path){
        if (path.empty() || path[0] == '#')
            return true;
        const std::string lower = toLower(path);
        return lower.rfind("data:", 0) == 0 || lower.rfind("lua://", 0) == 0 || lower.rfind("about:", 0) == 0 ||
               lower.rfind("javascript:", 0) == 0;
    }

    std::string extensionOf(const std::string& path){
        const size_t slash = path.find_last_of('/');
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            return "";
        return toLower(path.substr(dot + 1));
    }

    HtmlUIReference::Kind kindFromExtension(const std::string& path);

    HtmlUIFileLog::Scope*& currentScope(){
        static HtmlUIFileLog::Scope* scope = nullptr;
        return scope;
    }

    HtmlUIFileLog::MessageSink& messageObserver(){
        static HtmlUIFileLog::MessageSink observer;
        return observer;
    }

    std::mutex& lazyFilesMutex(){
        static std::mutex mutex;
        return mutex;
    }

    std::map<const void*, std::set<std::string>>& lazyFiles(){
        static std::map<const void*, std::set<std::string>> files;
        return files;
    }

    HtmlUIReference::Kind kindFromExtension(const std::string& path){
        const std::string ext = extensionOf(path);
        if (ext == "css" || ext == "rcss")
            return HtmlUIReference::STYLESHEET;
        if (ext == "svg")
            return HtmlUIReference::SVG;
        if (ext == "json" || ext == "lottie")
            return HtmlUIReference::LOTTIE;
        if (ext == "webm" || ext == "mkv" || ext == "mp4" || ext == "m4v" || ext == "mov" || ext == "ogv")
            return HtmlUIReference::VIDEO;
        if (ext == "ttf" || ext == "otf" || ext == "ttc" || ext == "woff" || ext == "woff2" || ext == "bitmapfont" || ext == "fnt")
            return HtmlUIReference::FONT;
        if (ext == "html" || ext == "htm" || ext == "rml")
            return HtmlUIReference::TEMPLATE;
        if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "tga" || ext == "bmp" || ext == "gif" ||
            ext == "webp" || ext == "psd" || ext == "hdr" || ext == "pic" || ext == "pnm" || ext == "ktx" || ext == "dds")
            return HtmlUIReference::IMAGE;
        return HtmlUIReference::OTHER;
    }

    // "a.css?v=2#x" -> "a.css" (web URLs carry queries; the engine reads plain files)
    std::string stripQuery(const std::string& path){
        const size_t cut = path.find_first_of("?#");
        return (cut == std::string::npos) ? path : path.substr(0, cut);
    }

    std::string decodeEntities(const std::string& value){
        std::string result;
        for (size_t i = 0; i < value.size(); i++){
            if (value[i] == '&'){
                const size_t end = value.find(';', i);
                if (end != std::string::npos && end - i <= 6){
                    const std::string name = value.substr(i + 1, end - i - 1);
                    const char* replacement = nullptr;
                    if (name == "amp") replacement = "&";
                    else if (name == "quot") replacement = "\"";
                    else if (name == "apos") replacement = "'";
                    else if (name == "lt") replacement = "<";
                    else if (name == "gt") replacement = ">";
                    if (replacement){
                        result += replacement;
                        i = end;
                        continue;
                    }
                }
            }
            result += value[i];
        }
        return result;
    }

    struct Scanner {
        const std::string& text;
        const std::string& fromFile;
        LineMap lines;
        std::vector<HtmlUIReference> out;

        Scanner(const std::string& text, const std::string& fromFile): text(text), fromFile(fromFile), lines(text){
        }

        // rootRelative: a path RmlUi opens as it is, from the asset root (RCSS's bare
        // @font-face src), not relative to the file that holds it
        bool add(const std::string& raw, size_t offset, HtmlUIReference::Kind kind, bool kindFromPath, bool rootRelative = false){
            const std::string value = trim(raw);
            if (isSkipped(value))
                return false;

            HtmlUIReference reference;
            reference.raw = value;
            reference.fromFile = fromFile;
            lines.locate(offset, reference.line, reference.column);
            reference.remote = isRemote(value);
            reference.path = reference.remote ? value : stripQuery(value);
            if (rootRelative && !reference.remote && !isAbsolutePath(reference.path) && reference.path.rfind("asset://", 0) != 0)
                reference.path = "asset://" + reference.path;
            if (kindFromPath){
                kind = kindFromExtension(reference.path);
                // a template is only a template where it is linked as one
                if (kind == HtmlUIReference::TEMPLATE)
                    kind = HtmlUIReference::OTHER;
            }
            reference.kind = kind;
            if (reference.path.empty())
                return false;
            out.push_back(reference);
            return true;
        }

        // ---- CSS ----

        // url( at `pos` (just after the parenthesis): the argument, its offset, and the end
        bool readFunctionArgument(size_t pos, size_t end, std::string& value, size_t& valueOffset, size_t& next, bool firstTokenOnly){
            while (pos < end && isSpace(text[pos])) pos++;
            if (pos >= end)
                return false;

            if (text[pos] == '"' || text[pos] == '\''){
                const char quote = text[pos];
                const size_t close = text.find(quote, pos + 1);
                if (close == std::string::npos || close >= end)
                    return false;
                value = text.substr(pos + 1, close - pos - 1);
                valueOffset = pos + 1;
                next = close + 1;
                return true;
            }

            size_t stop = pos;
            while (stop < end && text[stop] != ')' && !(firstTokenOnly && (isSpace(text[stop]) || text[stop] == ','))){
                stop++;
            }
            value = text.substr(pos, stop - pos);
            valueOffset = pos;
            next = stop;
            return true;
        }

        // a CSS range; declarationsOnly: the content of style="" (no selectors, no at-rules)
        void scanCss(size_t begin, size_t end, bool declarationsOnly){
            // at-rule of each open block: "font-face", "spritesheet", ... ("" for others)
            std::vector<std::string> blocks;
            std::string pendingAtRule;
            if (declarationsOnly)
                blocks.push_back("");

            size_t i = begin;
            while (i < end){
                const char c = text[i];

                if (c == '/' && i + 1 < end && text[i + 1] == '*'){
                    const size_t close = text.find("*/", i + 2);
                    i = (close == std::string::npos || close + 2 > end) ? end : close + 2;
                    continue;
                }

                if (c == '"' || c == '\''){
                    size_t close = i + 1;
                    while (close < end && text[close] != c && text[close] != '\n'){
                        if (text[close] == '\\') close++;
                        close++;
                    }
                    i = (close < end) ? close + 1 : end;
                    continue;
                }

                if (c == '@' && i + 1 < end && (std::isalpha((unsigned char)text[i + 1]) || text[i + 1] == '-')){
                    size_t nameEnd = i + 1;
                    while (nameEnd < end && isIdentChar(text[nameEnd])) nameEnd++;
                    const std::string name = toLower(text.substr(i + 1, nameEnd - i - 1));
                    i = nameEnd;

                    if (name == "import"){
                        size_t pos = i;
                        while (pos < end && isSpace(text[pos])) pos++;
                        std::string value;
                        size_t valueOffset = 0;
                        size_t next = pos;
                        if (startsWithNoCase(text, pos, "url(")){
                            if (readFunctionArgument(pos + 4, end, value, valueOffset, next, false))
                                add(value, valueOffset, HtmlUIReference::STYLESHEET, false);
                        }else if (pos < end && (text[pos] == '"' || text[pos] == '\'')){
                            if (readFunctionArgument(pos, end, value, valueOffset, next, false))
                                add(value, valueOffset, HtmlUIReference::STYLESHEET, false);
                        }
                        i = std::max(next, i);
                    }else{
                        pendingAtRule = name;
                    }
                    continue;
                }

                if (c == '{'){
                    blocks.push_back(pendingAtRule);
                    pendingAtRule.clear();
                    i++;
                    continue;
                }
                if (c == '}'){
                    if (!blocks.empty())
                        blocks.pop_back();
                    pendingAtRule.clear();
                    i++;
                    continue;
                }
                if (c == ';'){
                    pendingAtRule.clear();
                    i++;
                    continue;
                }

                if (isIdentChar(c) && (i == begin || !isIdentChar(text[i - 1]))){
                    size_t nameEnd = i;
                    while (nameEnd < end && isIdentChar(text[nameEnd])) nameEnd++;
                    const std::string name = toLower(text.substr(i, nameEnd - i));
                    const std::string block = blocks.empty() ? std::string() : blocks.back();

                    if (nameEnd < end && text[nameEnd] == '(' && (name == "url" || name == "image" || name == "svg")){
                        std::string value;
                        size_t valueOffset = 0;
                        size_t next = nameEnd + 1;
                        const bool decorator = (name != "url");
                        if (readFunctionArgument(nameEnd + 1, end, value, valueOffset, next, decorator)){
                            if (name == "url"){
                                HtmlUIReference::Kind kind = HtmlUIReference::OTHER;
                                bool fromPath = true;
                                if (block == "font-face"){ kind = HtmlUIReference::FONT; fromPath = false; }
                                else if (block == "spritesheet"){ kind = HtmlUIReference::IMAGE; fromPath = false; }
                                add(value, valueOffset, kind, fromPath);
                            }else if (value.find('.') != std::string::npos || value.find('/') != std::string::npos){
                                // image(name) may name a sprite instead of a file
                                const bool svg = (name == "svg") || extensionOf(stripQuery(value)) == "svg";
                                add(value, valueOffset, svg ? HtmlUIReference::SVG : HtmlUIReference::IMAGE, false);
                            }
                        }
                        i = std::max(next, nameEnd + 1);
                        continue;
                    }

                    // @spritesheet { src: atlas.png; } and RCSS's @font-face { src: font.ttf; }
                    if (name == "src" && (block == "spritesheet" || block == "font-face")){
                        size_t pos = nameEnd;
                        while (pos < end && isSpace(text[pos])) pos++;
                        if (pos < end && text[pos] == ':'){
                            pos++;
                            while (pos < end && isSpace(text[pos])) pos++;
                            if (!startsWithNoCase(text, pos, "url(")){
                                size_t stop = pos;
                                while (stop < end && text[stop] != ';' && text[stop] != '}') stop++;
                                std::string value = trim(text.substr(pos, stop - pos));
                                size_t valueOffset = pos;
                                if (value.size() >= 2 && (value[0] == '"' || value[0] == '\'') && value.back() == value[0]){
                                    value = value.substr(1, value.size() - 2);
                                    valueOffset++;
                                }
                                if (block == "font-face")
                                    add(value, valueOffset, HtmlUIReference::FONT, false, true);
                                else
                                    add(value, valueOffset, HtmlUIReference::IMAGE, false);
                                i = stop;
                                continue;
                            }
                        }
                    }

                    i = nameEnd;
                    continue;
                }

                i++;
            }
        }

        // ---- HTML / RML ----

        struct Attribute {
            std::string name;
            std::string value;
            size_t valueOffset = 0;
        };

        void scanDocument(){
            const size_t end = text.size();
            size_t i = 0;
            while (i < end){
                if (text.compare(i, 4, "<!--") == 0){
                    const size_t close = text.find("-->", i + 4);
                    i = (close == std::string::npos) ? end : close + 3;
                    continue;
                }
                if (text[i] != '<'){
                    i++;
                    continue;
                }
                if (i + 1 < end && (text[i + 1] == '!' || text[i + 1] == '?')){
                    // doctype, CDATA, processing instruction
                    if (text.compare(i, 9, "<![CDATA[") == 0){
                        const size_t close = text.find("]]>", i + 9);
                        i = (close == std::string::npos) ? end : close + 3;
                    }else{
                        const size_t close = text.find('>', i + 2);
                        i = (close == std::string::npos) ? end : close + 1;
                    }
                    continue;
                }
                if (i + 1 >= end || !std::isalpha((unsigned char)text[i + 1])){
                    i++;
                    continue;
                }

                size_t nameEnd = i + 1;
                while (nameEnd < end && (isIdentChar(text[nameEnd]) || text[nameEnd] == ':')) nameEnd++;
                const std::string tag = toLower(text.substr(i + 1, nameEnd - i - 1));

                std::vector<Attribute> attributes;
                size_t pos = nameEnd;
                bool selfClosing = false;
                while (pos < end && text[pos] != '>'){
                    if (isSpace(text[pos])){ pos++; continue; }
                    if (text[pos] == '/'){ selfClosing = true; pos++; continue; }
                    selfClosing = false;

                    size_t attrEnd = pos;
                    while (attrEnd < end && !isSpace(text[attrEnd]) && text[attrEnd] != '=' && text[attrEnd] != '>' && text[attrEnd] != '/') attrEnd++;
                    if (attrEnd == pos){ pos++; continue; }
                    Attribute attribute;
                    attribute.name = toLower(text.substr(pos, attrEnd - pos));
                    pos = attrEnd;
                    while (pos < end && isSpace(text[pos])) pos++;
                    if (pos < end && text[pos] == '='){
                        pos++;
                        while (pos < end && isSpace(text[pos])) pos++;
                        if (pos < end && (text[pos] == '"' || text[pos] == '\'')){
                            const char quote = text[pos];
                            const size_t close = text.find(quote, pos + 1);
                            const size_t stop = (close == std::string::npos) ? end : close;
                            attribute.value = text.substr(pos + 1, stop - pos - 1);
                            attribute.valueOffset = pos + 1;
                            pos = (close == std::string::npos) ? end : close + 1;
                        }else{
                            size_t stop = pos;
                            while (stop < end && !isSpace(text[stop]) && text[stop] != '>') stop++;
                            attribute.value = text.substr(pos, stop - pos);
                            attribute.valueOffset = pos;
                            pos = stop;
                        }
                    }
                    attributes.push_back(attribute);
                }
                const size_t tagEnd = (pos < end) ? pos + 1 : end;

                handleTag(tag, attributes);

                i = tagEnd;
                if (!selfClosing && (tag == "style" || tag == "script")){
                    const std::string closeTag = "</" + tag;
                    const size_t close = findNoCase(text, closeTag.c_str(), tagEnd, end);
                    const size_t contentEnd = (close == std::string::npos) ? end : close;
                    if (tag == "style")
                        scanCss(tagEnd, contentEnd, false);
                    i = contentEnd;
                }
            }
        }

        const Attribute* findAttribute(const std::vector<Attribute>& attributes, const char* name){
            for (const Attribute& attribute : attributes){
                if (attribute.name == name)
                    return &attribute;
            }
            return nullptr;
        }

        bool addAttribute(const Attribute* attribute, HtmlUIReference::Kind kind, bool kindFromPath){
            if (!attribute)
                return false;
            // references are located at the value's first character
            return add(decodeEntities(attribute->value), attribute->valueOffset, kind, kindFromPath);
        }

        void handleTag(const std::string& tag, const std::vector<Attribute>& attributes){
            if (tag == "link"){
                const Attribute* href = findAttribute(attributes, "href");
                const Attribute* type = findAttribute(attributes, "type");
                const Attribute* rel = findAttribute(attributes, "rel");
                const std::string typeValue = type ? toLower(trim(type->value)) : std::string();
                const std::string relValue = rel ? toLower(trim(rel->value)) : std::string();
                if (typeValue == "text/template"){
                    addAttribute(href, HtmlUIReference::TEMPLATE, false);
                }else if (typeValue == "text/rcss" || typeValue == "text/css" || relValue.find("stylesheet") != std::string::npos){
                    addAttribute(href, HtmlUIReference::STYLESHEET, false);
                }else{
                    addAttribute(href, HtmlUIReference::OTHER, true);
                }
            }else if (tag == "img"){
                // <img src="x.svg"> is an SVG; an <img> of an unknown extension is still an image
                if (addAttribute(findAttribute(attributes, "src"), HtmlUIReference::IMAGE, true) &&
                    out.back().kind == HtmlUIReference::OTHER){
                    out.back().kind = HtmlUIReference::IMAGE;
                }
            }else if (tag == "svg"){
                addAttribute(findAttribute(attributes, "src"), HtmlUIReference::SVG, false);
            }else if (tag == "lottie"){
                addAttribute(findAttribute(attributes, "src"), HtmlUIReference::LOTTIE, false);
            }else if (tag == "video"){
                addAttribute(findAttribute(attributes, "src"), HtmlUIReference::VIDEO, false);
                addAttribute(findAttribute(attributes, "poster"), HtmlUIReference::IMAGE, false);
            }else if (tag == "source"){
                // a <video>'s (a <picture>'s use srcset)
                addAttribute(findAttribute(attributes, "src"), HtmlUIReference::VIDEO, false);
            }else if (tag == "template"){
                addAttribute(findAttribute(attributes, "src"), HtmlUIReference::TEMPLATE, false);
            }else if (tag == "markdown"){
                // S17: <markdown src="notes.md">
                addAttribute(findAttribute(attributes, "src"), HtmlUIReference::OTHER, false);
            }

            if (const Attribute* style = findAttribute(attributes, "style")){
                const size_t begin = style->valueOffset;
                scanCss(begin, begin + style->value.size(), true);
            }
        }
    };

    bool readThroughData(const std::string& path, std::string* content){
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK)
            return false;
        if (content){
            const unsigned int length = data.length();
            if (length > 0)
                content->assign(reinterpret_cast<const char*>(data.getMemPtr()), length);
            else
                content->clear();
        }
        return true;
    }

}

std::string jasperengine::normalizeHtmlUIPath(const std::string& path){
    std::string value = path;
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.rfind("asset://", 0) == 0)
        value = value.substr(8);

    const bool absolute = !value.empty() && value[0] == '/';
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= value.size()){
        size_t end = value.find('/', start);
        if (end == std::string::npos)
            end = value.size();
        const std::string part = value.substr(start, end - start);
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

std::string jasperengine::resolveHtmlUIPath(const std::string& fromFile, const std::string& path, bool* outside){
    if (outside)
        *outside = false;

    std::string value = path;
    std::replace(value.begin(), value.end(), '\\', '/');

    if (value.rfind("asset://", 0) == 0)
        return normalizeHtmlUIPath(value);
    if (value.find("://") != std::string::npos || isAbsolutePath(value))
        return value;

    std::string base = fromFile;
    std::replace(base.begin(), base.end(), '\\', '/');
    if (base.rfind("asset://", 0) == 0)
        base = base.substr(8);
    const size_t slash = base.rfind('/');
    base = (slash == std::string::npos) ? std::string() : base.substr(0, slash + 1);

    // a document opened by an absolute path keeps its references absolute
    const bool absoluteBase = !base.empty() && base[0] == '/';

    std::vector<std::string> parts;
    const std::string joined = base + value;
    size_t start = 0;
    while (start <= joined.size()){
        size_t end = joined.find('/', start);
        if (end == std::string::npos)
            end = joined.size();
        const std::string part = joined.substr(start, end - start);
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

    std::string result = absoluteBase ? "/" : "";
    for (size_t i = 0; i < parts.size(); i++){
        if (i > 0)
            result += '/';
        result += parts[i];
    }
    return result;
}

std::vector<HtmlUIReference> jasperengine::scanHtmlUIDocumentText(const std::string& text, const std::string& fromFile){
    Scanner scanner(text, fromFile);
    scanner.scanDocument();
    return scanner.out;
}

std::vector<HtmlUIReference> jasperengine::scanHtmlUIStyleSheetText(const std::string& text, const std::string& fromFile){
    Scanner scanner(text, fromFile);
    scanner.scanCss(0, text.size(), false);
    return scanner.out;
}

std::vector<HtmlUIReference> jasperengine::collectHtmlUIReferences(const std::string& documentPath, const HtmlUIFileReader& reader){
    const HtmlUIFileReader read = reader ? reader : HtmlUIFileReader(readThroughData);

    std::vector<HtmlUIReference> result;
    std::set<std::string> visited;
    std::unordered_map<std::string, bool> exists;

    struct Pending {
        std::string path;
        bool styleSheet;
    };
    std::vector<Pending> pending;
    const std::string document = normalizeHtmlUIPath(documentPath);
    pending.push_back({document, false});
    visited.insert(document);

    // depth first, so references come out in reading order of each file
    while (!pending.empty()){
        const Pending current = pending.back();
        pending.pop_back();

        std::string content;
        if (!read(current.path, &content))
            continue;

        std::vector<HtmlUIReference> found = current.styleSheet ?
            scanHtmlUIStyleSheetText(content, current.path) :
            scanHtmlUIDocumentText(content, current.path);

        std::vector<Pending> children;
        for (HtmlUIReference& reference : found){
            if (!reference.remote){
                reference.absolute = isAbsolutePath(reference.path);
                bool outside = false;
                reference.path = resolveHtmlUIPath(current.path, reference.path, &outside);
                reference.outsideAssets = outside;

                auto it = exists.find(reference.path);
                if (it == exists.end())
                    it = exists.emplace(reference.path, read(reference.path, nullptr)).first;
                reference.missing = !it->second;

                const bool nested = (reference.kind == HtmlUIReference::STYLESHEET || reference.kind == HtmlUIReference::TEMPLATE);
                if (nested && !reference.missing && !reference.outsideAssets && visited.insert(reference.path).second){
                    children.push_back({reference.path, reference.kind == HtmlUIReference::STYLESHEET});
                }
            }
            result.push_back(reference);
        }

        // the stack pops the last pushed first
        for (auto it = children.rbegin(); it != children.rend(); ++it){
            pending.push_back(*it);
        }
    }

    return result;
}

const char* jasperengine::getHtmlUIReferenceKindName(HtmlUIReference::Kind kind){
    switch (kind){
        case HtmlUIReference::STYLESHEET: return "stylesheet";
        case HtmlUIReference::IMAGE: return "image";
        case HtmlUIReference::FONT: return "font";
        case HtmlUIReference::SVG: return "svg";
        case HtmlUIReference::LOTTIE: return "lottie";
        case HtmlUIReference::TEMPLATE: return "template";
        case HtmlUIReference::VIDEO: return "video";
        case HtmlUIReference::OTHER: default: return "other";
    }
}

HtmlUIReference::Kind jasperengine::getHtmlUIReferenceKind(const std::string& path){
    return kindFromExtension(stripQuery(path));
}

HtmlUIFileLog::Scope::Scope(std::vector<std::string>* files, MessageSink messages):
        previous(currentScope()), files(files), messages(std::move(messages)){
    currentScope() = this;
}

HtmlUIFileLog::Scope::~Scope(){
    currentScope() = previous;
}

void HtmlUIFileLog::recordFile(const std::string& path){
    Scope* scope = currentScope();
    if (!scope || !scope->files || path.empty())
        return;

    const std::string normalized = normalizeHtmlUIPath(path);
    if (std::find(scope->files->begin(), scope->files->end(), normalized) == scope->files->end())
        scope->files->push_back(normalized);
}

void HtmlUIFileLog::recordMessage(Level level, const std::string& text){
    Scope* scope = currentScope();
    if (scope && scope->messages){
        scope->messages(level, text);
        return;
    }

    // a copy: the observer may log
    MessageSink observer = messageObserver();
    if (observer)
        observer(level, text);
}

void HtmlUIFileLog::setMessageObserver(MessageSink observer){
    messageObserver() = std::move(observer);
}

void HtmlUIFileLog::recordLazyFile(const void* renderer, const std::string& path){
    if (!renderer || path.empty())
        return;
    std::lock_guard<std::mutex> lock(lazyFilesMutex());
    lazyFiles()[renderer].insert(normalizeHtmlUIPath(path));
}

std::vector<std::string> HtmlUIFileLog::getLazyFiles(const void* renderer){
    std::lock_guard<std::mutex> lock(lazyFilesMutex());
    auto it = lazyFiles().find(renderer);
    if (it == lazyFiles().end())
        return {};
    return std::vector<std::string>(it->second.begin(), it->second.end());
}

void HtmlUIFileLog::forgetRenderer(const void* renderer){
    std::lock_guard<std::mutex> lock(lazyFilesMutex());
    lazyFiles().erase(renderer);
}

bool jasperengine::parseHtmlUIMessageLocation(const std::string& text, std::string& file, int& line){
    // "XML parse error on line 12 of ui/menu.html"
    const size_t onLine = text.find("line ");
    if (onLine != std::string::npos){
        size_t pos = onLine + 5;
        size_t digits = pos;
        while (digits < text.size() && std::isdigit((unsigned char)text[digits])) digits++;
        if (digits > pos && text.compare(digits, 4, " of ") == 0){
            size_t start = digits + 4;
            size_t end = start;
            while (end < text.size() && !isSpace(text[end])) end++;
            while (end > start && (text[end - 1] == '.' || text[end - 1] == ',')) end--;
            if (end > start){
                file = normalizeHtmlUIPath(text.substr(start, end - start));
                line = std::atoi(text.substr(pos, digits - pos).c_str());
                return true;
            }
        }
    }

    // "<path>.<ext>:<line>" anywhere: Log::ParseError and the stylesheet parser's "at %s:%d"
    for (size_t colon = text.find(':'); colon != std::string::npos; colon = text.find(':', colon + 1)){
        size_t digits = colon + 1;
        while (digits < text.size() && std::isdigit((unsigned char)text[digits])) digits++;
        if (digits == colon + 1)
            continue;

        size_t start = colon;
        while (start > 0 && !isSpace(text[start - 1]) && text[start - 1] != '\'' && text[start - 1] != '"' && text[start - 1] != '(')
            start--;
        const std::string candidate = text.substr(start, colon - start);
        if (candidate.empty() || extensionOf(candidate).empty())
            continue;

        file = normalizeHtmlUIPath(candidate);
        line = std::atoi(text.substr(colon + 1, digits - colon - 1).c_str());
        return true;
    }

    return false;
}
