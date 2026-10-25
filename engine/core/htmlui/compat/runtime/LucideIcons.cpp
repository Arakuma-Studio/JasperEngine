// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/LucideIcons.h"

#include "Log.h"

#include "htmlui_lucide.h"
#include "stb_image.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    constexpr uint32_t kFormatVersion = 1;

    // The inflated pack: views into `payload`
    struct Pack {
        std::string payload;
        std::string version;
        struct Icon {
            std::string_view name;
            std::string_view markup;
        };
        std::vector<Icon> icons;                                   // sorted by name
        std::vector<std::pair<std::string_view, uint32_t>> aliases;  // sorted by name
        // Lucide's lookup key: toPascalCase of every icon and alias name -> icon index
        std::vector<std::pair<std::string, uint32_t>> pascal;
        bool valid = false;
    };

    class Reader {
    public:
        Reader(const std::string& data): data(data) {}

        bool u16(uint16_t& value){
            if (position + 2 > data.size())
                return false;
            value = (uint16_t)((uint8_t)data[position] | ((uint8_t)data[position + 1] << 8));
            position += 2;
            return true;
        }

        bool u32(uint32_t& value){
            if (position + 4 > data.size())
                return false;
            value = 0;
            for (int i = 3; i >= 0; i--)
                value = (value << 8) | (uint8_t)data[position + i];
            position += 4;
            return true;
        }

        bool bytes(size_t count, std::string_view& out){
            if (count > data.size() - position)
                return false;
            out = std::string_view(data).substr(position, count);
            position += count;
            return true;
        }

        bool string(std::string_view& out){
            uint16_t length = 0;
            return u16(length) && bytes(length, out);
        }

        size_t remaining() const { return data.size() - position; }

    private:
        const std::string& data;
        size_t position = 0;
    };

    uint32_t readU32(const unsigned char* data){
        return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    }

    bool fail(std::string* error, const std::string& message){
        if (error)
            *error = message;
        return false;
    }

    bool parsePack(const unsigned char* data, size_t size, Pack& pack, std::string* error){
        pack = Pack();
        if (!data || size < 12)
            return fail(error, "the pack is empty or truncated");
        if (std::memcmp(data, "LUCP", 4) != 0)
            return fail(error, "the pack does not start with LUCP");
        if (readU32(data + 4) != kFormatVersion)
            return fail(error, "unknown pack format version " + std::to_string(readU32(data + 4)));
        const uint32_t payloadSize = readU32(data + 8);
        if (size - 12 > (size_t)INT32_MAX)
            return fail(error, "the pack is too large");

        int inflatedSize = 0;
        char* inflated = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(data + 12), (int)(size - 12), &inflatedSize);
        if (!inflated)
            return fail(error, "the pack's zlib stream is corrupt");
        pack.payload.assign(inflated, (size_t)inflatedSize);
        std::free(inflated);
        if (pack.payload.size() != payloadSize)
            return fail(error, "the inflated payload has " + std::to_string(pack.payload.size()) + " bytes, the header says " +
                               std::to_string(payloadSize));

        Reader reader(pack.payload);
        uint32_t iconCount = 0, aliasCount = 0;
        std::string_view version;
        if (!reader.u32(iconCount) || !reader.u32(aliasCount) || !reader.string(version))
            return fail(error, "the payload header is truncated");
        pack.version = std::string(version);

        struct Entry {
            std::string_view name;
            uint32_t offset, length;
        };
        std::vector<Entry> entries;
        entries.reserve(iconCount);
        for (uint32_t i = 0; i < iconCount; i++){
            Entry entry{};
            if (!reader.string(entry.name) || !reader.u32(entry.offset) || !reader.u32(entry.length))
                return fail(error, "the icon table is truncated");
            if (i > 0 && !(entries.back().name < entry.name))
                return fail(error, "the icon table is not sorted");
            entries.push_back(entry);
        }
        pack.aliases.reserve(aliasCount);
        for (uint32_t i = 0; i < aliasCount; i++){
            std::string_view name;
            uint32_t icon = 0;
            if (!reader.string(name) || !reader.u32(icon))
                return fail(error, "the alias table is truncated");
            if (icon >= iconCount)
                return fail(error, "alias '" + std::string(name) + "' names icon " + std::to_string(icon) + " of " + std::to_string(iconCount));
            if (i > 0 && !(pack.aliases.back().first < name))
                return fail(error, "the alias table is not sorted");
            pack.aliases.emplace_back(name, icon);
        }
        uint32_t markupSize = 0;
        std::string_view markup;
        if (!reader.u32(markupSize) || !reader.bytes(markupSize, markup) || reader.remaining() != 0)
            return fail(error, "the markup block is truncated or followed by extra bytes");

        pack.icons.reserve(entries.size());
        for (const Entry& entry : entries){
            if ((size_t)entry.offset + entry.length > markup.size())
                return fail(error, "icon '" + std::string(entry.name) + "' points outside the markup");
            pack.icons.push_back({entry.name, markup.substr(entry.offset, entry.length)});
        }

        // icons first, so an alias never hides an icon with the same PascalCase key
        pack.pascal.reserve(pack.icons.size() + pack.aliases.size());
        for (uint32_t i = 0; i < pack.icons.size(); i++)
            pack.pascal.emplace_back(LucideIcons::toPascalCase(pack.icons[i].name), i);
        for (const auto& alias : pack.aliases)
            pack.pascal.emplace_back(LucideIcons::toPascalCase(alias.first), alias.second);
        std::stable_sort(pack.pascal.begin(), pack.pascal.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
        pack.pascal.erase(std::unique(pack.pascal.begin(), pack.pascal.end(), [](const auto& a, const auto& b){ return a.first == b.first; }),
                          pack.pascal.end());

        pack.valid = true;
        return true;
    }


    const Pack& pack(){
        static Pack instance;
        static std::once_flag once;
        std::call_once(once, [](){
            if (embedded::lucidePackSize == 0){
                Log::warn("[HTML UI] this build has no Lucide icon pack (engine/libs/lucide/lucide.pack was not embedded)");
                return;
            }
            std::string error;
            if (!parsePack(embedded::lucidePack, embedded::lucidePackSize, instance, &error))
                Log::error("[HTML UI] the built-in Lucide icon pack is invalid: %s", error.c_str());
        });
        return instance;
    }

    const Pack::Icon* lookup(std::string_view name){
        const Pack& p = pack();
        if (!p.valid || name.empty())
            return nullptr;
        const std::string key = LucideIcons::toPascalCase(name);
        const auto it = std::lower_bound(p.pascal.begin(), p.pascal.end(), key, [](const auto& entry, const std::string& k){ return entry.first < k; });
        if (it == p.pascal.end() || it->first != key)
            return nullptr;
        return &p.icons[it->second];
    }

    char asciiLower(char c){
        return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }

    char asciiUpper(char c){
        return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }

    size_t editDistance(std::string_view a, std::string_view b){
        std::vector<size_t> previous(b.size() + 1), current(b.size() + 1);
        for (size_t j = 0; j <= b.size(); j++)
            previous[j] = j;
        for (size_t i = 1; i <= a.size(); i++){
            current[0] = i;
            for (size_t j = 1; j <= b.size(); j++){
                const size_t substitution = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
                current[j] = std::min({previous[j] + 1, current[j - 1] + 1, substitution});
            }
            std::swap(previous, current);
        }
        return previous[b.size()];
    }

    std::string escapeAttribute(std::string_view value){
        std::string out;
        out.reserve(value.size());
        for (char c : value){
            switch (c){
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                default: out += c; break;
            }
        }
        return out;
    }

    // a JavaScript object spread: a new key goes last, an existing key keeps its place
    void assign(std::vector<LucideAttribute>& attributes, const std::string& name, const std::string& value){
        for (LucideAttribute& attribute : attributes){
            if (attribute.name == name){
                attribute.value = value;
                return;
            }
        }
        attributes.push_back({name, value});
    }

}

bool LucideIcons::available(){
    return pack().valid;
}

const std::string& LucideIcons::version(){
    return pack().version;
}

size_t LucideIcons::iconCount(){
    return pack().icons.size();
}

size_t LucideIcons::aliasCount(){
    return pack().aliases.size();
}

std::vector<std::string> LucideIcons::names(){
    std::vector<std::string> result;
    result.reserve(pack().icons.size());
    for (const Pack::Icon& icon : pack().icons)
        result.emplace_back(icon.name);
    return result;
}

std::string LucideIcons::resolve(std::string_view name){
    const Pack::Icon* icon = lookup(name);
    return icon ? std::string(icon->name) : std::string();
}

std::optional<std::string_view> LucideIcons::find(std::string_view name){
    const Pack::Icon* icon = lookup(name);
    if (!icon)
        return std::nullopt;
    return icon->markup;
}

std::vector<std::string> LucideIcons::closest(std::string_view name, size_t count){
    const Pack& p = pack();
    std::vector<std::pair<size_t, std::string_view>> scored;
    if (!p.valid || count == 0)
        return {};
    const std::string kebab = toKebabCase(name);
    scored.reserve(p.icons.size());
    for (const Pack::Icon& icon : p.icons)
        scored.emplace_back(editDistance(kebab, icon.name), icon.name);
    const size_t keep = std::min(count, scored.size());
    std::partial_sort(scored.begin(), scored.begin() + (std::ptrdiff_t)keep, scored.end());
    std::vector<std::string> result;
    for (size_t i = 0; i < keep; i++)
        result.emplace_back(scored[i].second);
    return result;
}

std::vector<LucideAttribute> LucideIcons::svgAttributes(std::string_view name, const std::vector<LucideAttribute>& elementAttributes){
    // packages/lucide/src/replaceElement.ts: { ...defaultAttributes, 'data-lucide': iconName,
    // ...ariaProps, ...attrs, ...elementAttrs }, then class = mergeClasses('lucide',
    // `lucide-${iconName}`, ...elementClassNames)
    std::vector<LucideAttribute> attributes = {
        {"xmlns", "http://www.w3.org/2000/svg"},
        {"width", "24"},
        {"height", "24"},
        {"viewBox", "0 0 24 24"},
        {"fill", "none"},
        {"stroke", "currentColor"},
        {"stroke-width", "2"},
        {"stroke-linecap", "round"},
        {"stroke-linejoin", "round"},
    };
    const std::string iconName(name);
    assign(attributes, "data-lucide", iconName);

    // hasA11yProp (@lucide/shared)
    bool accessible = false;
    for (const LucideAttribute& attribute : elementAttributes){
        if (attribute.name.compare(0, 5, "aria-") == 0 || attribute.name == "role" || attribute.name == "title")
            accessible = true;
    }
    if (!accessible)
        assign(attributes, "aria-hidden", "true");

    std::string elementClass;
    bool hasClass = false;
    for (const LucideAttribute& attribute : elementAttributes){
        assign(attributes, attribute.name, attribute.value);
        if (attribute.name == "class"){
            elementClass = attribute.value;
            hasClass = true;
        }
    }

    // mergeClasses: non-empty after trim, first occurrence only, joined with spaces
    std::vector<std::string> classes = {"lucide", "lucide-" + iconName};
    if (hasClass){
        size_t start = 0;
        while (true){
            const size_t space = elementClass.find(' ', start);
            classes.push_back(elementClass.substr(start, space == std::string::npos ? std::string::npos : space - start));
            if (space == std::string::npos)
                break;
            start = space + 1;
        }
    }
    std::vector<std::string> merged;
    for (const std::string& candidate : classes){
        const size_t first = candidate.find_first_not_of(" \t\n\r\f\v");
        if (candidate.empty() || first == std::string::npos)
            continue;
        if (std::find(merged.begin(), merged.end(), candidate) == merged.end())
            merged.push_back(candidate);
    }
    std::string className;
    for (const std::string& value : merged)
        className += (className.empty() ? "" : " ") + value;
    const size_t begin = className.find_first_not_of(" \t\n\r\f\v");
    const size_t end = className.find_last_not_of(" \t\n\r\f\v");
    className = begin == std::string::npos ? std::string() : className.substr(begin, end - begin + 1);
    if (!className.empty())
        assign(attributes, "class", className);

    return attributes;
}

std::optional<std::string> LucideIcons::svgMarkup(std::string_view name, const std::vector<LucideAttribute>& elementAttributes){
    const std::optional<std::string_view> shapes = find(name);
    if (!shapes)
        return std::nullopt;
    std::string markup = "<svg";
    for (const LucideAttribute& attribute : svgAttributes(name, elementAttributes))
        markup += " " + attribute.name + "=\"" + escapeAttribute(attribute.value) + "\"";
    markup += ">";
    markup += *shapes;
    markup += "</svg>";
    return markup;
}

std::string LucideIcons::buildEmbeddedHeader(const unsigned char* data, size_t size){
    static const char* hex = "0123456789abcdef";
    std::string content = "// Generated from engine/libs/lucide/lucide.pack. Do not edit.\n\n";
    content += "#pragma once\n\n#include <cstddef>\n\n";
    content += "namespace jasperengine { namespace htmlui { namespace compat { namespace embedded {\n\n";
    content += "static const unsigned char lucidePack[] = {";
    content.reserve(content.size() + size * 5 + 200);
    for (size_t i = 0; i < size; i++){
        const unsigned char byte = data[i];
        content += "0x";
        content += hex[byte >> 4];
        content += hex[byte & 0xF];
        content += ',';
    }
    content += "0x00};\n";
    content += "static const size_t lucidePackSize = " + std::to_string(size) + ";\n\n} } } }\n";
    return content;
}

std::string LucideIcons::buildEmbeddedHeader(){
    return buildEmbeddedHeader(embedded::lucidePack, embedded::lucidePackSize);
}

std::string LucideIcons::toPascalCase(std::string_view name){
    // toCamelCase: separators ('-', '_', control characters and spaces) drop and uppercase the
    // next character (unless at the start); the first character is lowercased
    std::string camel;
    bool upperNext = false;
    for (char c : name){
        if (c == '-' || c == '_' || ((unsigned char)c <= ' ')){
            upperNext = !camel.empty();
            continue;
        }
        if (camel.empty())
            camel += asciiLower(c);
        else
            camel += upperNext ? asciiUpper(c) : c;
        upperNext = false;
    }
    if (!camel.empty())
        camel[0] = asciiUpper(camel[0]);
    return camel;
}

std::string LucideIcons::toKebabCase(std::string_view name){
    // string.replace(/([a-z0-9])([A-Z])/g, '$1-$2').toLowerCase()
    std::string kebab;
    kebab.reserve(name.size() + 4);
    for (size_t i = 0; i < name.size(); i++){
        const char c = name[i];
        if (i > 0 && c >= 'A' && c <= 'Z'){
            const char previous = name[i - 1];
            if ((previous >= 'a' && previous <= 'z') || (previous >= '0' && previous <= '9'))
                kebab += '-';
        }
        kebab += asciiLower(c);
    }
    return kebab;
}
