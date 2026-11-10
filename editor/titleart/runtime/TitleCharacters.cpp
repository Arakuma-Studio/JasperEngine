// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleCharacters.h"

#include "TitleRuntimeResources.h"
#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtHash.h"
#include "titleart/TitleArtIO.h"

#include <algorithm>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
using TJson = titleart::Json;

namespace {

    bool validCodepoint(uint32_t cp) {
        return cp <= 0x10ffff && (cp < 0xd800 || cp > 0xdfff);
    }

    // a C0 control, DEL or a byte order mark: what a text file holds besides its characters
    bool fileControl(uint32_t cp) {
        return cp < 0x20 || cp == 0x7f || cp == 0xfeff;
    }

    // UTF-8 decoding that skips invalid bytes (a text file of any origin)
    void decodeUtf8(std::string_view text, std::vector<uint32_t>& out) {
        size_t i = 0;
        while (i < text.size()) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            uint32_t cp = 0;
            size_t extra = 0;
            if (c < 0x80) { cp = c; }
            else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; extra = 1; }
            else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; extra = 2; }
            else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; extra = 3; }
            else { i++; continue; }
            if (i + extra >= text.size()) {
                i++;
                continue;
            }
            bool ok = true;
            for (size_t k = 1; k <= extra; k++) {
                const unsigned char d = static_cast<unsigned char>(text[i + k]);
                if ((d & 0xc0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | (d & 0x3f);
            }
            // overlong forms are invalid
            static const uint32_t minimum[4] = {0, 0x80, 0x800, 0x10000};
            if (!ok || cp < minimum[extra] || !validCodepoint(cp)) {
                i++;
                continue;
            }
            out.push_back(cp);
            i += extra + 1;
        }
    }

    // ---- presets ----

    void addJoyoKanji(TitleCharacterSet& out) {
        const std::string_view text = titleJoyoKanjiText();
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string_view::npos) end = text.size();
            const std::string_view line = text.substr(start, end - start);
            if (!line.empty() && line[0] != '#') {
                std::vector<uint32_t> cps;
                decodeUtf8(line, cps);
                for (uint32_t cp : cps) {
                    if (cp > 0x20 && cp != 0x3000 && cp != 0x7f) out.add(cp);
                }
            }
            start = end + 1;
        }
    }

    const std::map<std::string, TitleCharacterSet>& presetTable() {
        static const std::map<std::string, TitleCharacterSet> table = [] {
            std::map<std::string, TitleCharacterSet> t;
            TitleCharacterSet digits;
            // 0-9 + - . , : / % × x $ € £ ¥ and space, as UTF-8 escapes
            digits.addUtf8("0123456789+-.,:/%" "\xc3\x97" "x$" "\xe2\x82\xac" "\xc2\xa3" "\xc2\xa5" " ");
            t["digits"] = digits;
            TitleCharacterSet latin;
            latin.addRange(0x20, 0x7e);
            t["latin"] = latin;
            TitleCharacterSet extended = latin;
            extended.addRange(0xa0, 0x24f);
            extended.addRange(0x1e00, 0x1eff);
            extended.addRange(0x2000, 0x206f);
            extended.addRange(0x20a0, 0x20cf);
            t["latin_extended"] = extended;
            TitleCharacterSet kana;
            kana.addRange(0x3000, 0x303f);
            kana.addRange(0x3040, 0x309f);
            kana.addRange(0x30a0, 0x30ff);
            kana.addRange(0x31f0, 0x31ff);
            kana.addRange(0xff00, 0xffef);
            t["kana"] = kana;
            TitleCharacterSet joyo;
            addJoyoKanji(joyo);
            t["joyo_kanji"] = joyo;
            return t;
        }();
        return table;
    }

    void addStrings(const TJson& value, TitleCharacterSet& out) {
        if (value.is_string()) {
            out.addUtf8(value.get_ref<const std::string&>(), true);
        } else if (value.is_object() || value.is_array()) {
            for (const TJson& item : value) addStrings(item, out);
        }
    }

}

// ---- TitleCharacterSet ----

void TitleCharacterSet::mergeSorted(const std::vector<uint32_t>& sorted) {
    if (sorted.empty()) return;
    std::vector<uint32_t> merged;
    merged.reserve(codepoints.size() + sorted.size());
    std::set_union(codepoints.begin(), codepoints.end(), sorted.begin(), sorted.end(), std::back_inserter(merged));
    codepoints = std::move(merged);
}

bool TitleCharacterSet::add(uint32_t codepoint) {
    if (!validCodepoint(codepoint)) return false;
    auto it = std::lower_bound(codepoints.begin(), codepoints.end(), codepoint);
    if (it == codepoints.end() || *it != codepoint) codepoints.insert(it, codepoint);
    return true;
}

void TitleCharacterSet::addRange(uint32_t first, uint32_t last) {
    std::vector<uint32_t> range;
    for (uint32_t cp = first; cp <= last && cp <= 0x10ffff; cp++) {
        if (validCodepoint(cp)) range.push_back(cp);
    }
    mergeSorted(range);
}

void TitleCharacterSet::addUtf8(std::string_view text, bool skipControls) {
    std::vector<uint32_t> cps;
    decodeUtf8(text, cps);
    if (skipControls) cps.erase(std::remove_if(cps.begin(), cps.end(), fileControl), cps.end());
    std::sort(cps.begin(), cps.end());
    cps.erase(std::unique(cps.begin(), cps.end()), cps.end());
    mergeSorted(cps);
}

void TitleCharacterSet::merge(const TitleCharacterSet& other) {
    mergeSorted(other.codepoints);
}

bool TitleCharacterSet::contains(uint32_t codepoint) const {
    return std::binary_search(codepoints.begin(), codepoints.end(), codepoint);
}

std::vector<uint32_t> TitleCharacterSet::missingFrom(std::string_view text) const {
    std::vector<uint32_t> cps, out;
    decodeUtf8(text, cps);
    for (uint32_t cp : cps) {
        if (!contains(cp) && std::find(out.begin(), out.end(), cp) == out.end()) out.push_back(cp);
    }
    return out;
}

std::string TitleCharacterSet::hash() const {
    std::string bytes;
    bytes.reserve(codepoints.size() * 3);
    for (uint32_t cp : codepoints) {
        bytes.push_back(static_cast<char>(cp & 0xff));
        bytes.push_back(static_cast<char>((cp >> 8) & 0xff));
        bytes.push_back(static_cast<char>((cp >> 16) & 0xff));
    }
    static const char* hex = "0123456789abcdef";
    const uint64_t h = titleart::fnv1a64(bytes);
    std::string out(16, '0');
    for (int i = 0; i < 16; i++) out[(size_t)i] = hex[(h >> (60 - 4 * i)) & 0xf];
    return out;
}

std::string TitleCharacterSet::toUtf8() const {
    std::string out;
    out.reserve(codepoints.size() * 2);
    for (uint32_t cp : codepoints) out += titleUtf8Of(cp);
    return out;
}

std::string editor::titleUtf8Of(uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
    return out;
}

// ---- presets and documents ----

const std::vector<std::string>& editor::titleCharacterPresetNames() {
    static const std::vector<std::string> names = {"digits", "latin", "latin_extended", "kana", "joyo_kanji"};
    return names;
}

bool editor::titleCharacterPreset(const std::string& name, TitleCharacterSet& out) {
    const auto& table = presetTable();
    auto it = table.find(name);
    if (it == table.end()) return false;
    out = it->second;
    return true;
}

TitleCharacterFileReader editor::titleCharacterFileReader(const titleart::AssetResolver& assets) {
    return [&assets](const titleart::TitleAssetRef& ref, std::string& text, std::string& error) {
        // the reference kinds name fonts, images, vectors and maps; a text file resolves as the
        // Image kind does (a path or GUID with no type of its own, TitleArtRefs)
        const titleart::ResolvedTitleAsset resolved = assets.resolve(ref, titleart::TitleAssetKind::Image);
        if (!resolved.found) {
            error = "the Custom File " + (ref.path.empty() ? ref.guid : ref.path) + " is missing";
            return false;
        }
        std::vector<uint8_t> bytes;
        if (!assets.readBytes(resolved, bytes, &error)) return false;
        text.assign(bytes.begin(), bytes.end());
        return true;
    };
}

void editor::addTitleDocumentText(const titleart::TitleArtDoc& doc, TitleCharacterSet& out) {
    titleart::forEachLayer(doc.root, [&](const TJson& layer, const TJson*, int) {
        if (titleart::layerTypeOf(layer) == titleart::LayerType::Text) {
            if (auto it = layer.find("text"); it != layer.end() && it->is_string()) out.addUtf8(it->get_ref<const std::string&>(), true);
        }
        if (auto it = layer.find("translations"); it != layer.end()) addStrings(*it, out);
        return true;
    });
    if (auto it = doc.root.find("translations"); it != doc.root.end()) addStrings(*it, out);
    // S87 R17: each Language's extra characters for runtime text
    if (auto it = doc.root.find("languages"); it != doc.root.end() && it->is_array()) {
        for (const TJson& language : *it) {
            if (language.is_object() && language.contains("characters") && language["characters"].is_string()) {
                out.addUtf8(language["characters"].get_ref<const std::string&>(), true);
            }
        }
    }
    // an exposed text parameter's default is its targets' current value (S80 §6.1)
    auto exposed = doc.root.find("exposed");
    if (exposed == doc.root.end() || !exposed->is_array()) return;
    for (const TJson& param : *exposed) {
        if (!param.is_object() || param.value("type", std::string()) != "text") continue;
        auto targets = param.find("targets");
        if (targets == param.end() || !targets->is_array()) continue;
        for (const TJson& target : *targets) {
            if (!target.is_object()) continue;
            const TJson* layer = titleart::findLayer(doc.root, target.value("layer", std::string()));
            if (!layer) continue;
            const TJson* value = titleart::getTitleArtField(*layer, "layer", target.value("field", std::string()));
            if (value && value->is_string()) out.addUtf8(value->get_ref<const std::string&>(), true);
        }
    }
}

TitleCharacterSet editor::titleDocumentCharacters(const titleart::TitleArtDoc& doc, const TitleCharacterFileReader& readFile,
                                                  TitleCharactersReport* report) {
    TitleCharactersReport local;
    TitleCharactersReport& r = report ? *report : local;
    r = TitleCharactersReport();
    TitleCharacterSet set;
    const TJson* characters = nullptr;
    if (auto runtime = doc.root.find("runtime"); runtime != doc.root.end() && runtime->is_object()) {
        if (auto it = runtime->find("characters"); it != runtime->end() && it->is_object()) characters = &*it;
    }
    // a document normalized before the runtime member existed has its defaults (§6.2)
    const TJson defaults = TJson{{"presets", TJson::array({"latin"})}, {"custom", ""}, {"custom_file", nullptr}, {"document_text", true}};
    const TJson& c = characters ? *characters : defaults;

    if (auto presets = c.find("presets"); presets != c.end() && presets->is_array()) {
        for (const TJson& name : *presets) {
            if (!name.is_string()) continue;
            TitleCharacterSet preset;
            if (!titleCharacterPreset(name.get<std::string>(), preset)) {
                r.warnings.push_back("unknown Characters preset '" + name.get<std::string>() + "' ignored");
                continue;
            }
            r.presets.push_back(name.get<std::string>());
            set.merge(preset);
        }
    }
    if (auto custom = c.find("custom"); custom != c.end() && custom->is_string()) {
        TitleCharacterSet added;
        added.addUtf8(custom->get_ref<const std::string&>(), true);
        r.customCount = added.size();
        r.declared.merge(added);
        set.merge(added);
    }
    if (auto file = c.find("custom_file"); file != c.end()) {
        const titleart::TitleAssetRef ref = titleart::decodeAssetRef(*file);
        if (!ref.empty()) {
            std::string text, error;
            if (!readFile) {
                r.warnings.push_back("the Custom File " + (ref.path.empty() ? ref.guid : ref.path) + " was not read");
            } else if (!readFile(ref, text, error)) {
                r.warnings.push_back(error.empty() ? "the Custom File cannot be read" : error);
            } else {
                TitleCharacterSet added;
                added.addUtf8(text, true);
                r.fileCount = added.size();
                r.declared.merge(added);
                set.merge(added);
            }
        }
    }
    if (c.value("document_text", true)) {
        TitleCharacterSet text;
        addTitleDocumentText(doc, text);
        r.documentCount = text.size();
        set.merge(text);
    }
    return set;
}

size_t editor::titleDocumentCharacterCount(const titleart::TitleArtDoc& doc, const TitleCharacterFileReader& readFile) {
    return titleDocumentCharacters(doc, readFile).size();
}
