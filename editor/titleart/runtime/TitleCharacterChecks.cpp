// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleCharacterChecks.h"

#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtChecks.h"
#include "util/FontFace.h"

#include "hb.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using TJson = titleart::Json;
namespace fs = std::filesystem;

namespace {

    // White space, controls, format characters (joiners, marks of direction) and variation
    // selectors draw nothing of their own (as S80's glyph_missing skips them); unassigned code
    // points are no font's to have (the preset ranges hold some: U+3040, U+FFEF)
    bool drawsNothing(uint32_t cp) {
        if ((cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0100 && cp <= 0xe01ef)) return true;
        switch (hb_unicode_general_category(hb_unicode_funcs_get_default(), cp)) {
            case HB_UNICODE_GENERAL_CATEGORY_CONTROL:
            case HB_UNICODE_GENERAL_CATEGORY_FORMAT:
            case HB_UNICODE_GENERAL_CATEGORY_UNASSIGNED:
            case HB_UNICODE_GENERAL_CATEGORY_SURROGATE:
            case HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR:
            case HB_UNICODE_GENERAL_CATEGORY_LINE_SEPARATOR:
            case HB_UNICODE_GENERAL_CATEGORY_PARAGRAPH_SEPARATOR:
                return true;
            default:
                return false;
        }
    }

    std::string str(const TJson& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    std::string charList(const std::vector<uint32_t>& cps, size_t max = 12) {
        std::string chars, codes;
        for (size_t i = 0; i < cps.size() && i < max; i++) {
            char code[16];
            std::snprintf(code, sizeof(code), "U+%04X", cps[i]);
            chars += (i ? " " : "") + titleUtf8Of(cps[i]);
            codes += std::string(i ? ", " : "") + code;
        }
        if (cps.size() > max) {
            chars += " ...";
            codes += ", ...";
        }
        return chars + " (" + codes + ")";
    }

    // the exposed parameters of type text, by name
    std::set<std::string> exposedTextNames(const titleart::TitleArtDoc& doc) {
        std::set<std::string> names;
        auto exposed = doc.root.find("exposed");
        if (exposed == doc.root.end() || !exposed->is_array()) return names;
        for (const TJson& param : *exposed) {
            if (param.is_object() && str(param, "type") == "text") names.insert(str(param, "name"));
        }
        return names;
    }

    // ---- the Lua lexer: names, symbols and decoded string literals, with their lines ----

    struct Token {
        enum Kind { Name, String, Symbol } kind;
        std::string text;
        int line;
    };

    int longBracketLevel(const std::string& s, size_t i) {
        if (i >= s.size() || s[i] != '[') return -1;
        size_t j = i + 1;
        int level = 0;
        while (j < s.size() && s[j] == '=') { level++; j++; }
        return (j < s.size() && s[j] == '[') ? level : -1;
    }

    std::string readLongBracket(const std::string& s, size_t& i, int level, int& line) {
        i += (size_t)level + 2;
        // a newline right after the opening bracket is not part of the string
        if (i < s.size() && s[i] == '\r') i++;
        if (i < s.size() && s[i] == '\n') { i++; line++; }
        const std::string close = "]" + std::string((size_t)level, '=') + "]";
        const size_t end = s.find(close, i);
        const size_t stop = end == std::string::npos ? s.size() : end;
        std::string text = s.substr(i, stop - i);
        line += (int)std::count(text.begin(), text.end(), '\n');
        i = end == std::string::npos ? s.size() : end + close.size();
        return text;
    }

    // a quoted string at s[i] (the quote), escapes decoded; i ends after it
    std::string readQuoted(const std::string& s, size_t& i, int& line) {
        const char quote = s[i++];
        std::string text;
        while (i < s.size() && s[i] != quote) {
            const char c = s[i];
            if (c == '\n') break;   // an unfinished string ends at its line
            if (c != '\\' || i + 1 >= s.size()) {
                text += c;
                i++;
                continue;
            }
            const char e = s[i + 1];
            i += 2;
            switch (e) {
                case 'n': text += '\n'; break;
                case 't': text += '\t'; break;
                case 'r': text += '\r'; break;
                case 'a': text += '\a'; break;
                case 'b': text += '\b'; break;
                case 'f': text += '\f'; break;
                case 'v': text += '\v'; break;
                case '\n': text += '\n'; line++; break;
                case 'z':
                    while (i < s.size() && std::isspace((unsigned char)s[i])) {
                        if (s[i] == '\n') line++;
                        i++;
                    }
                    break;
                case 'x': {
                    unsigned value = 0;
                    int digits = 0;
                    while (digits < 2 && i < s.size() && std::isxdigit((unsigned char)s[i])) {
                        value = value * 16 + (unsigned)(std::isdigit((unsigned char)s[i]) ? s[i] - '0' : (std::tolower((unsigned char)s[i]) - 'a' + 10));
                        i++;
                        digits++;
                    }
                    text += static_cast<char>(value);
                    break;
                }
                case 'u': {
                    if (i < s.size() && s[i] == '{') {
                        i++;
                        uint32_t value = 0;
                        while (i < s.size() && std::isxdigit((unsigned char)s[i])) {
                            value = value * 16 + (uint32_t)(std::isdigit((unsigned char)s[i]) ? s[i] - '0' : (std::tolower((unsigned char)s[i]) - 'a' + 10));
                            i++;
                        }
                        if (i < s.size() && s[i] == '}') i++;
                        text += titleUtf8Of(value);
                    }
                    break;
                }
                default:
                    if (std::isdigit((unsigned char)e)) {
                        unsigned value = (unsigned)(e - '0');
                        for (int digits = 1; digits < 3 && i < s.size() && std::isdigit((unsigned char)s[i]); digits++) value = value * 10 + (unsigned)(s[i++] - '0');
                        text += static_cast<char>(value & 0xff);
                    } else {
                        text += e;   // \\ \" \' and anything Lua refuses
                    }
            }
        }
        if (i < s.size() && s[i] == quote) i++;
        return text;
    }

    std::vector<Token> lexLua(const std::string& s) {
        std::vector<Token> tokens;
        int line = 1;
        size_t i = 0;
        while (i < s.size()) {
            const char c = s[i];
            if (c == '\n') { line++; i++; continue; }
            if (std::isspace((unsigned char)c)) { i++; continue; }
            if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
                i += 2;
                const int level = longBracketLevel(s, i);
                if (level >= 0) readLongBracket(s, i, level, line);
                else while (i < s.size() && s[i] != '\n') i++;
                continue;
            }
            if (c == '[') {
                const int level = longBracketLevel(s, i);
                if (level >= 0) {
                    const int at = line;
                    tokens.push_back({Token::String, readLongBracket(s, i, level, line), at});
                    continue;
                }
            }
            if (c == '"' || c == '\'') {
                const int at = line;
                tokens.push_back({Token::String, readQuoted(s, i, line), at});
                continue;
            }
            if (std::isalpha((unsigned char)c) || c == '_') {
                const size_t start = i;
                while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
                tokens.push_back({Token::Name, s.substr(start, i - start), line});
                continue;
            }
            if (std::isdigit((unsigned char)c)) {
                while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '.')) i++;
                tokens.push_back({Token::Symbol, "0", line});
                continue;
            }
            // two-character operators, so `==` is never an assignment
            static const char* pairs[] = {"==", "~=", "<=", ">=", "..", "::"};
            bool paired = false;
            for (const char* pair : pairs) {
                if (i + 1 < s.size() && s[i] == pair[0] && s[i + 1] == pair[1]) {
                    tokens.push_back({Token::Symbol, pair, line});
                    i += 2;
                    paired = true;
                    break;
                }
            }
            if (paired) continue;
            tokens.push_back({Token::Symbol, std::string(1, c), line});
            i++;
        }
        return tokens;
    }

    bool isSymbol(const std::vector<Token>& t, size_t i, const char* text) {
        return i < t.size() && t[i].kind == Token::Symbol && t[i].text == text;
    }

    bool isTitleSource(const std::vector<Token>& t, size_t i) {
        return i < t.size() && t[i].kind == Token::Name && (t[i].text == "getTitleArt" || t[i].text == "TitleArt") && isSymbol(t, i + 1, "(");
    }

    // a setParameter value that is not text: a colour or an asset path
    bool notText(const std::string& value) {
        if (!value.empty() && value[0] == '#' && (value.size() == 4 || value.size() == 7) &&
            std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isxdigit(c); }))
            return true;
        const size_t dot = value.rfind('.');
        return value.find('/') != std::string::npos && dot != std::string::npos && dot + 1 < value.size() &&
               value.find(' ') == std::string::npos;
    }

    struct LiteralCache {
        std::map<std::string, std::pair<std::string, std::vector<TitleLuaLiteral>>> files;   // path -> (stamp, literals)
    };
    LiteralCache& literalCache() {
        static LiteralCache cache;
        return cache;
    }

    TitleLuaLiteralProvider& provider() {
        static TitleLuaLiteralProvider p;
        return p;
    }

    // the project folder of a check's document: its absolute file minus its project-relative path
    fs::path projectRootOf(const TitleCheckContext& ctx) {
        if (ctx.file.empty() || ctx.path.empty()) return fs::path();
        fs::path root = ctx.file;
        const fs::path relative(ctx.path);
        for (auto it = relative.begin(); it != relative.end(); ++it) root = root.parent_path();
        return root;
    }

    void addStrings(const TJson& value, TitleCharacterSet& out) {
        if (value.is_string()) out.addUtf8(value.get_ref<const std::string&>(), true);
        else if (value.is_object() || value.is_array()) for (const TJson& item : value) addStrings(item, out);
    }

    TJson codepointsJson(const std::vector<uint32_t>& cps) {
        TJson out = TJson::array();
        for (uint32_t cp : cps) out.push_back(TJson{{"codepoint", cp}, {"char", titleUtf8Of(cp)}});
        return out;
    }

    void runCharactersCheck(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out) {
        if (!ctx.assets) {
            TitleCheckResult r;
            r.id = "characters";
            r.status = TitleCheckStatus::Skip;
            r.message = "no asset resolver: not measured";
            r.method = "font coverage";
            out.push_back(r);
            return;
        }
        TitleCharactersReport report;
        const TitleCharacterSet set = titleDocumentCharacters(ctx.doc, titleCharacterFileReader(*ctx.assets), &report);
        const std::string title = ctx.path.empty() ? std::string("this title") : ctx.path;

        // coverage: what the dynamic layers are given one by one (Custom, Custom File, their texts
        // and translations) that their fonts cannot draw; a preset's gaps are counted, not warned
        // about, and the other layers' texts are glyphs_present's
        {
            TitleCheckResult r;
            r.id = "characters.coverage";
            r.method = "each code point of Custom, Custom File and the dynamic layers' texts against the font, the fallback "
                       "fonts, then the built-in face of each layer the exposed text reaches; presets' gaps counted only";
            TitleCharacterSet named = report.declared;
            for (const std::string& id : titleDynamicTextLayers(ctx.doc)) {
                const TJson* layer = titleart::findLayer(ctx.doc.root, id);
                if (!layer) continue;
                named.addUtf8(str(*layer, "text"), true);
                if (auto translations = layer->find("translations"); translations != layer->end()) addStrings(*translations, named);
            }
            if (auto translations = ctx.doc.root.find("translations"); translations != ctx.doc.root.end()) addStrings(*translations, named);
            const std::vector<TitleMissingCharacters> missing = findTitleMissingCharacters(ctx.doc, named, *ctx.assets, title);
            size_t presetGaps = 0;
            for (const TitleMissingCharacters& m : findTitleMissingCharacters(ctx.doc, set, *ctx.assets, title)) presetGaps += m.codepoints.size();
            size_t count = 0;
            TJson details = TJson::array(), ops = TJson::array();
            std::vector<std::pair<titleart::ResolvedTitleAsset, std::shared_ptr<FontFace>>> projectFonts;
            bool projectFontsLoaded = false;
            for (const TitleMissingCharacters& m : missing) {
                count += m.codepoints.size();
                details.push_back(TJson{{"code", m.code}, {"layer", m.layer}, {"font", m.font}, {"chars", codepointsJson(m.codepoints)},
                                        {"message", m.message}});
                if (!projectFontsLoaded) {
                    projectFontsLoaded = true;
                    for (const titleart::ResolvedTitleAsset& asset : ctx.assets->projectFonts()) {
                        if (projectFonts.size() >= 64) break;
                        if (std::shared_ptr<FontFace> face = ctx.assets->loadFont(asset, 0, nullptr)) projectFonts.push_back({asset, face});
                    }
                }
                for (const auto& [asset, face] : projectFonts) {
                    if (!std::all_of(m.codepoints.begin(), m.codepoints.end(), [&](uint32_t cp) { return face->hasCodepoint(cp); })) continue;
                    const TJson* layer = titleart::findLayer(ctx.doc.root, m.layer);
                    TJson fallbacks = layer && layer->contains("fallback_fonts") ? (*layer)["fallback_fonts"] : TJson::array();
                    fallbacks.push_back(TJson{{"path", asset.assetPath}, {"guid", asset.guid}});
                    ops.push_back(TJson{{"op", "set"}, {"layer", "id:" + m.layer}, {"values", TJson{{"fallback_fonts", fallbacks}}}});
                    break;
                }
            }
            r.status = missing.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Warn;
            r.measured = TJson{{"characters", set.size()}, {"presets", report.presets}, {"missing", count},
                               {"preset_gaps", presetGaps - std::min(presetGaps, count)}};
            if (!missing.empty()) r.measured["details"] = details;
            if (!report.warnings.empty()) r.measured["warnings"] = report.warnings;
            r.threshold = TJson{{"missing", 0}};
            r.message = missing.empty() ? "the fonts draw every character named in Characters and the document"
                                        : missing.front().message;
            if (!ops.empty()) r.fix = TJson{{"ops", ops}};
            out.push_back(std::move(r));
        }

        // literals: what scripts give this title's text parameters stays inside Characters
        {
            TitleCheckResult r;
            r.id = "characters.literals";
            r.method = "string literals of setText, text = and setParameter on TitleArt values in the project's Lua scripts "
                       "(best effort), for this title's exposed text parameters";
            const fs::path root = projectRootOf(ctx);
            if (root.empty()) {
                r.status = TitleCheckStatus::Skip;
                r.message = "no project folder: scripts not read";
                out.push_back(std::move(r));
                return;
            }
            const std::set<std::string> names = exposedTextNames(ctx.doc);
            std::vector<TitleLuaLiteral> relevant;
            const std::vector<TitleLuaLiteral> literals = provider() ? provider()(root) : collectTitleLuaLiterals(root);
            for (const TitleLuaLiteral& literal : literals) {
                if (names.count(literal.parameter)) relevant.push_back(literal);
            }
            const std::vector<TitleCharsWarning> warnings = titleLiteralCharsWarnings(relevant, set, title);
            TitleCharacterSet outside;
            TJson details = TJson::array();
            for (const TitleCharsWarning& w : warnings) {
                for (uint32_t cp : w.codepoints) outside.add(cp);
                details.push_back(TJson{{"code", w.code}, {"file", w.file}, {"line", w.line}, {"parameter", w.parameter},
                                        {"chars", codepointsJson(w.codepoints)}, {"message", w.message}});
            }
            r.status = warnings.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Warn;
            r.measured = TJson{{"literals", relevant.size()}, {"outside", outside.size()}};
            if (!warnings.empty()) r.measured["details"] = details;
            r.threshold = TJson{{"outside", 0}};
            if (warnings.empty()) {
                r.message = relevant.empty() ? "no script literal reaches this title's text parameters"
                                             : "every script literal for this title is inside its Characters";
            } else {
                r.message = warnings.front().message;
                std::string custom;
                if (auto runtime = ctx.doc.root.find("runtime"); runtime != ctx.doc.root.end() && runtime->is_object()) {
                    if (auto c = runtime->find("characters"); c != runtime->end() && c->is_object()) custom = str(*c, "custom");
                }
                r.fix = TJson{{"ops", TJson::array({TJson{{"op", "set"}, {"values", TJson{{"runtime.characters.custom", custom + outside.toUtf8()}}}}})}};
            }
            out.push_back(std::move(r));
        }
    }

}

// ==================================================================== font chains

TitleFontChain editor::titleLayerFontChain(const TJson& layer, const titleart::AssetResolver& assets) {
    TitleFontChain chain;
    auto add = [&](const TJson& refJson, int faceIndex) {
        const titleart::TitleAssetRef ref = titleart::decodeAssetRef(refJson);
        if (ref.empty()) return;
        const titleart::ResolvedTitleAsset resolved = assets.resolve(ref, titleart::TitleAssetKind::Font);
        std::shared_ptr<FontFace> face = resolved.found ? assets.loadFont(resolved, faceIndex, nullptr) : nullptr;
        if (!face) {
            if (chain.missing.empty()) chain.missing = ref.path.empty() ? ref.guid : ref.path;
            return;
        }
        chain.faces.push_back(face);
        chain.names.push_back(resolved.assetPath);
    };
    if (auto font = layer.find("font"); font != layer.end()) add(*font, layer.value("face_index", 0));
    if (auto fallbacks = layer.find("fallback_fonts"); fallbacks != layer.end() && fallbacks->is_array()) {
        for (const TJson& ref : *fallbacks) add(ref, 0);
    }
    if (std::shared_ptr<FontFace> builtin = assets.builtinFont()) {
        chain.faces.push_back(builtin);
        chain.names.push_back("the built-in face");
    }
    return chain;
}

std::vector<uint32_t> editor::titleChainMissing(const TitleFontChain& chain, const TitleCharacterSet& chars) {
    std::vector<uint32_t> missing;
    for (uint32_t cp : chars.list()) {
        if (drawsNothing(cp)) continue;
        bool covered = false;
        for (const std::shared_ptr<FontFace>& face : chain.faces) {
            if (face->hasCodepoint(cp)) {
                covered = true;
                break;
            }
        }
        if (!covered) missing.push_back(cp);
    }
    return missing;
}

std::vector<std::string> editor::titleDynamicTextLayers(const titleart::TitleArtDoc& doc) {
    std::vector<std::string> ids;
    auto exposed = doc.root.find("exposed");
    if (exposed != doc.root.end() && exposed->is_array()) {
        for (const TJson& param : *exposed) {
            if (!param.is_object() || str(param, "type") != "text") continue;
            auto targets = param.find("targets");
            if (targets == param.end() || !targets->is_array()) continue;
            for (const TJson& target : *targets) {
                const std::string id = target.is_object() ? str(target, "layer") : std::string();
                const TJson* layer = titleart::findLayer(doc.root, id);
                if (layer && titleart::layerTypeOf(*layer) == titleart::LayerType::Text &&
                    std::find(ids.begin(), ids.end(), id) == ids.end())
                    ids.push_back(id);
            }
        }
    }
    if (!ids.empty()) return ids;
    titleart::forEachLayer(doc.root, [&](const TJson& layer, const TJson*, int) {
        if (titleart::layerTypeOf(layer) == titleart::LayerType::Text) ids.push_back(str(layer, "id"));
        return true;
    });
    return ids;
}

std::vector<TitleMissingCharacters> editor::findTitleMissingCharacters(const titleart::TitleArtDoc& doc, const TitleCharacterSet& chars,
                                                                       const titleart::AssetResolver& assets, const std::string& title) {
    std::vector<TitleMissingCharacters> out;
    for (const std::string& id : titleDynamicTextLayers(doc)) {
        const TJson* layer = titleart::findLayer(doc.root, id);
        if (!layer) continue;
        const TitleFontChain chain = titleLayerFontChain(*layer, assets);
        const std::vector<uint32_t> missing = titleChainMissing(chain, chars);
        if (missing.empty()) continue;
        TitleMissingCharacters m;
        m.title = title;
        m.layer = id;
        m.layerName = str(*layer, "name");
        m.font = chain.names.empty() ? std::string("no font") : chain.names.front();
        m.codepoints = missing;
        std::string fonts;
        for (size_t i = 0; i < chain.names.size(); i++) fonts += (i ? ", then " : "") + chain.names[i];
        if (!chain.missing.empty()) fonts += "; " + chain.missing + " is missing";
        m.message = "Title Art " + title + ", layer '" + m.layerName + "': no font of its chain (" + fonts + ") has " +
                    std::to_string(missing.size()) + " of its Characters: " + charList(missing) +
                    "; add a font that has them to the layer's Fallback Fonts, or remove them from Characters.";
        out.push_back(std::move(m));
    }
    return out;
}

// ==================================================================== the Lua scan

std::vector<TitleLuaLiteral> editor::scanTitleLuaLiterals(const std::string& source, const std::string& file) {
    const std::vector<Token> t = lexLua(source);
    std::set<std::string> titles;
    std::vector<TitleLuaLiteral> out;
    // the names a TitleArt was assigned to: `name = <expression calling TitleArt( / getTitleArt(>`
    // on its line
    for (size_t i = 0; i < t.size(); i++) {
        if (!isTitleSource(t, i)) continue;
        for (size_t j = i; j-- > 0 && t[j].line == t[i].line;) {
            if (isSymbol(t, j, "=") && j > 0 && t[j - 1].kind == Token::Name) {
                titles.insert(t[j - 1].text);
                break;
            }
        }
    }
    // a receiver is a TitleArt when it is one of those names, or a call chained on a TitleArt
    // source on the same line (`go:getTitleArt():setText(..)`)
    auto titleReceiver = [&](size_t receiver) {
        if (t[receiver].kind == Token::Name) return titles.count(t[receiver].text) > 0;
        if (!isSymbol(t, receiver, ")")) return false;
        for (size_t j = receiver; j-- > 0 && t[j].line == t[receiver].line;) {
            if (isTitleSource(t, j)) return true;
        }
        return false;
    };
    for (size_t i = 1; i + 1 < t.size(); i++) {
        if (t[i].kind != Token::Name) continue;
        const bool method = isSymbol(t, i - 1, ":");
        const bool field = isSymbol(t, i - 1, ".");
        if ((!method && !field) || i < 2 || !titleReceiver(i - 2)) continue;
        size_t a = i + 1;
        if (field && t[i].text == "text" && isSymbol(t, a, "=") && a + 1 < t.size() && t[a + 1].kind == Token::String) {
            out.push_back({file, t[a + 1].line, "text =", "text", t[a + 1].text});
            continue;
        }
        if (!method) continue;
        const bool parens = isSymbol(t, a, "(");
        if (parens) a++;
        if (t[i].text == "setText" && a < t.size() && t[a].kind == Token::String) {
            out.push_back({file, t[a].line, "setText", "text", t[a].text});
        } else if (t[i].text == "setParameter" && parens && a + 2 < t.size() && t[a].kind == Token::String && isSymbol(t, a + 1, ",") &&
                   t[a + 2].kind == Token::String && !notText(t[a + 2].text)) {
            out.push_back({file, t[a + 2].line, "setParameter", t[a].text, t[a + 2].text});
        }
    }
    return out;
}

std::vector<TitleCharsWarning> editor::titleLiteralCharsWarnings(const std::vector<TitleLuaLiteral>& literals, const TitleCharacterSet& set,
                                                                 const std::string& title) {
    std::vector<TitleCharsWarning> out;
    for (const TitleLuaLiteral& literal : literals) {
        std::vector<uint32_t> outside = set.missingFrom(literal.value);
        outside.erase(std::remove_if(outside.begin(), outside.end(), drawsNothing), outside.end());
        if (outside.empty()) continue;
        TitleCharsWarning w;
        w.file = literal.file;
        w.line = literal.line;
        w.parameter = literal.parameter;
        w.codepoints = outside;
        const std::string call = literal.call == "setParameter" ? "setParameter(\"" + literal.parameter + "\", ...)" : literal.call;
        w.message = literal.file + ":" + std::to_string(literal.line) + ": " + call + " gives " + title + " " +
                    std::to_string(outside.size()) + " character(s) outside its Characters, " + charList(outside) +
                    ", which the build's fonts will not have: add them to Characters > Custom.";
        out.push_back(std::move(w));
    }
    return out;
}

std::vector<TitleLuaLiteral> editor::collectTitleLuaLiterals(const fs::path& projectRoot) {
    std::vector<TitleLuaLiteral> out;
    std::error_code ec;
    LiteralCache& cache = literalCache();
    std::set<std::string> seen;
    for (fs::recursive_directory_iterator it(projectRoot, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::path& path = it->path();
        if (it->is_directory(ec)) {
            // .jasperengine, .git and the other hidden folders hold no project script
            if (!path.filename().empty() && path.filename().string()[0] == '.') it.disable_recursion_pending();
            continue;
        }
        if (path.extension() != ".lua" || !it->is_regular_file(ec)) continue;
        const std::string key = path.string();
        const auto time = fs::last_write_time(path, ec);
        const std::string stamp = std::to_string(fs::file_size(path, ec)) + ":" + std::to_string(static_cast<long long>(time.time_since_epoch().count()));
        seen.insert(key);
        auto cached = cache.files.find(key);
        if (cached == cache.files.end() || cached->second.first != stamp) {
            std::ifstream file(path, std::ios::binary);
            const std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            cache.files[key] = {stamp, scanTitleLuaLiterals(source, path.lexically_relative(projectRoot).generic_string())};
            cached = cache.files.find(key);
        }
        out.insert(out.end(), cached->second.second.begin(), cached->second.second.end());
    }
    for (auto it = cache.files.begin(); it != cache.files.end();) {
        const bool inProject = it->first.rfind(projectRoot.string(), 0) == 0;
        it = inProject && !seen.count(it->first) ? cache.files.erase(it) : std::next(it);
    }
    return out;
}

void editor::setTitleLuaLiteralProvider(TitleLuaLiteralProvider p) {
    provider() = std::move(p);
}

void editor::registerTitleCharactersCheck() {
    static bool done = false;
    if (done) return;
    done = true;
    CheckDef def;
    def.id = "characters";
    def.kind = TitleCheckKind::Document;
    def.owner = "S81";
    def.description = "Characters: the fonts draw them, and script literals stay inside them";
    def.run = runCharactersCheck;
    TitleArtChecks::registerCheck(std::move(def));
}
